// --world club host (+ --screenshot-crowd) — lifted VERBATIM from main() (#28).
#include "world_host_common.h"
#include "../scene.h"
#include "../club1127.h"
#include "../club_bedrock.h"                 // solid-earth encasement (underground)
#include "../jukebox.h"                     // Club Jukebox — Tim's personal-use "Self Radio"
#include "../crowd.h"
#include "../player.h"
#include "../asset_root.h"
#include "../audio_root.h"                 // resolveAudio (the committed club track)
#include "../chat_tree.h"                   // feat/club-npcs: ChatTreeSystem + drawChatTreeUi
#include "../timeline.h"                    // globalTimeline() (chat-tree axis fx context)
#include "../settings_io.h"                // readAudioSettings (respect music vol/on)
#include <cstdlib>                          // getenv (X3_CLUB_SEQ clip capture)
#include <cstdio>                           // snprintf (clip frame paths)
#include "engine/audio/IAudioSystem.h"

namespace x3 { namespace apphost {

int hostClub(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;
    const bool ddgiForce = hc.ddgiForce;
    const bool crowdShot = hc.crowdShot;
    const std::string& crowdShotPath = hc.crowdShotPath;

    // ==== VERBATIM host body ====
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

        // ==== SOLID-EARTH ENCASEMENT (fix/club-underground-sky) =================
        // The club is ~800 m (engine Y=-200) underground, but it was authored as a
        // hollow box in the void: with the sky off you saw the engine's dark HDR
        // clear ("grayness") through the ceiling hole, and noclipping out dropped
        // you into black nothing. Carve the club out of a MASSIVE solid-earth body
        // instead: a big double-sided, non-colliding rock block that fills
        // everything outside the club cavity, spanning from below the floor UP to
        // the surface ground plane (Y=0, host_surface_start kGroundY) and WIDE
        // enough to underlie the whole city above (regions.json `city`: anchor
        // (-200,0,425), half 750 -> X[-950,550] Z[-325,1175]) plus a wide margin.
        // So noclipping out in ANY direction — including straight up toward the
        // surface — stays buried in hundreds of meters of visible rock. The block
        // is COARSE (6 bulk boxes) so it costs almost nothing; detail only matters
        // at the carved cavity walls. This is the club's first carved cavity — the
        // descent / tunnels / Complex carve out of the SAME earth later (see
        // club_bedrock.h for the carve recipe + CSG upgrade path).
        //
        // ARCHITECTURE FOLLOW-UP (@13700k): true surface<->club continuity (walk/dig
        // from the city DOWN through the strata to the club) means the club should
        // eventually be a REGION in the streamed region-graph world (--world
        // streamed), not this standalone --world club. For now the massive earth
        // lives in the club world so noclip already shows solid earth up toward the
        // surface; unifying club into the streamed/city world is the real
        // one-continuous-dig follow-up.
        // Blue point lights of the Salvari crystal hollows — merged into the light
        // set the host pushes each frame (club.update re-pushes only club lights).
        std::vector<x3::rhi::PointLight> bedrockLights;
        {
            const auto& cs = club.stats();
            x3::game::BedrockConfig bc;
            // Cavity = the club footprint + clearance (a few m of excavation before
            // rock; keeps the rock off the club's own walls/booth/speakers/orb).
            bc.cavMinX = cs.roomMinX - 8.0f;   bc.cavMaxX = cs.roomMaxX + 8.0f;
            bc.cavMinZ = cs.roomMinZ - 14.0f;  bc.cavMaxZ = cs.roomMaxZ + 12.0f;
            bc.cavMinY = cs.floorY   - 3.0f;   bc.cavMaxY = cs.ceilingY + 4.0f;
            // Earth block: down 60 m below the club, UP to the surface (Y=0), and
            // out well beyond the city footprint in both horizontal axes.
            bc.earthMinY = cs.floorY - 60.0f;  bc.earthMaxY = 0.0f;
            bc.earthMinX = -1150.0f;           bc.earthMaxX = 750.0f;
            bc.earthMinZ = -525.0f;            bc.earthMaxZ = 1375.0f;
            const int nRock = x3::game::buildClubBedrock(cscene, *device, *cphys, bc, &bedrockLights);
            x3::logInfo("--world club: bedrock encasement — " + std::to_string(nRock) +
                        " solid-earth blocks (cavity ~" +
                        std::to_string((int)(bc.cavMaxX - bc.cavMinX)) + "x" +
                        std::to_string((int)(bc.cavMaxZ - bc.cavMinZ)) + "m, earth up to Y=0)");

            // ==== EARTH TUNNEL NETWORK (feat/earth-tunnels) ====================
            // Bore a WALKABLE strata descent down through the same solid earth from
            // near the surface (Y~=-3) to the club floor (Y=-200), a bottom connector
            // into the club's east elevator doorway, and 3 offshoot passages (one
            // holds a Salvari crystal hollow). The descent is the vertical spine the
            // elevator rides; the network is the first of "a tunnel network to dig
            // out" (grow it via the recipe in club_bedrock.h). Its mood + crystal
            // lights ride the SAME distance-culled bedrockLights channel.
            x3::game::TunnelConfig tc;
            tc.tint[0]=bc.tint[0]; tc.tint[1]=bc.tint[1]; tc.tint[2]=bc.tint[2]; tc.tint[3]=bc.tint[3];
            tc.emissive[0]=bc.emissive[0]; tc.emissive[1]=bc.emissive[1];
            tc.emissive[2]=bc.emissive[2]; tc.emissive[3]=bc.emissive[3];
            tc.bottomY   = cs.floorY;                        // club floor (-200)
            tc.topY      = -3.0f;                            // just under the surface
            tc.shaftX    = cs.roomMaxX + 22.0f;              // solid earth east of the club
            tc.shaftZ    = cs.roomMaxZ - 2.75f;              // aligned with the E doorway Z
            tc.clubDoorX = cs.roomMaxX;                      // club east face
            tc.clubDoorZ = cs.roomMaxZ - 2.75f;              // authored elevator-doorway Z
            const int nTun = x3::game::buildEarthTunnels(cscene, *device, *cphys, tc, &bedrockLights);
            x3::logInfo("--world club: earth tunnels — " + std::to_string(nTun) +
                        " pieces (walkable switchback descent Y=-3..-200 + connector + 3 offshoots)");
        }
        // The earth reaches ~200 m UP to the surface and ~1 km OUT under the city,
        // so the 200 m default far plane would clip the distant rock walls to the
        // dark HDR clear ("grayness") — the very void we're killing. Push the club's
        // far plane out so the whole earth body renders (space/streamed use 15-60 km;
        // 4 km easily covers this block from inside the club AND from an external
        // vantage). Depth precision at the tiny club is unaffected in practice.
        device->setCameraFar(4000.0f);

        // LIVING WORLD: the dance-floor crowd — 14 club-goers in neon-tinted
        // knots around the floor/bars, bobbing to the beat (CrowdSystem).
        x3::game::CrowdSystem clubCrowd;
        {
            const auto& cs = club.stats();
            x3::game::CrowdConfig ccfg;
            ccfg.count   = 14;
            ccfg.centerX = (cs.roomMinX + cs.roomMaxX) * 0.5f;
            ccfg.centerZ = (cs.roomMinZ + cs.roomMaxZ) * 0.5f;
            ccfg.groundY = x3::game::Club1127World::kClubY;
            ccfg.radius  = std::min(cs.roomMaxX - cs.roomMinX,
                                    cs.roomMaxZ - cs.roomMinZ) * 0.5f - 1.4f;
            // Hangout knots: dance-floor center + toward the DJ end + the bars.
            ccfg.points = { ccfg.centerX,        ccfg.centerZ,
                            ccfg.centerX,        ccfg.centerZ - 7.0f,
                            ccfg.centerX,        ccfg.centerZ + 7.0f,
                            ccfg.centerX - 4.0f, ccfg.centerZ,
                            ccfg.centerX + 4.0f, ccfg.centerZ - 3.0f };
            ccfg.dance    = true;     // they sway/bob to the beat
            // MAX-OUT (Tim: 'work on those dancers'): the pastel BOX agents are
            // retired from the dance floor — Club1127World now owns ten real
            // skinned GLB dancers with beat choreography. Keep a THIN box crowd
            // (4) as dim far-corner wallflowers at the south end so the room
            // feels populated beyond the floor (facility civilians still use
            // the full CrowdSystem elsewhere).
            // Box agents retired ENTIRELY in the club (they wandered back onto
            // the floor as day-glo boxes next to the real dancers).
            ccfg.count = 0;
            (void)clubCrowd;
        }

        // Apply the neon/UV point-light set once (the orbiting spot/ring lights are
        // re-pushed each frame by club.update()). The club has NO sky (deep interior).
        // pushLights() concatenates the club's own (already-animated) lights with the
        // Salvari crystal-hollow lights and re-pushes the combined set — call it AFTER
        // club.update() each frame (update re-pushes only the club lights, which would
        // otherwise drop the crystal glow). The crystals gently BREATHE (cheap: a sine
        // on the premultiplied blue).
        // NOTE: the club already fills the device's 64-point-light cap, so crystal
        // lights are only added when the camera is NEAR a hollow (prepended, so if
        // the total spills past 64 it's a far/irrelevant CLUB light that drops, not
        // the nearby crystal). In the club proper no hollow is near, so the club
        // keeps its full 64-light show untouched.
        auto pushLights = [&](float tsec, float cx, float cy, float cz) {
            const auto& cl = club.pointLights();
            std::vector<x3::rhi::PointLight> all;
            all.reserve(cl.size() + bedrockLights.size());
            for (size_t i = 0; i < bedrockLights.size(); ++i) {
                const x3::rhi::PointLight& s = bedrockLights[i];
                const float dx = s.pos[0]-cx, dy = s.pos[1]-cy, dz = s.pos[2]-cz;
                if (dx*dx + dy*dy + dz*dz > 50.0f * 50.0f) continue;   // only near hollows
                x3::rhi::PointLight l = s;
                const float b = 0.78f + 0.22f * std::sin(tsec * 1.6f + (float)i * 1.7f);
                l.color[0] *= b; l.color[1] *= b; l.color[2] *= b;
                all.push_back(l);
            }
            all.insert(all.end(), cl.begin(), cl.end());
            device->setPointLights(all.data(), (uint32_t)all.size());
        };
        { const auto sp0 = club.spawn(); pushLights(0.0f, sp0.x, sp0.y, sp0.z); }

        // ==== INTERIOR ATMOSPHERE (fix/club-relight) ============================
        // THE CLUB WAS A BLACK VOID. It disabled the sky and then set NOTHING else —
        // so iblValid=0 and every surface the point lights didn't directly hit fell
        // to near-black (the SAME "metals go black" root the facility/rifthub/level1
        // already fixed: an interior needs an ENVIRONMENT to be lit, not just lamps).
        // Give it the interior recipe those scenes use (setIblProbe + setIblIntensity
        // + a low ambient floor), tuned for a DARK-BUT-READABLE neon club instead of
        // an overcast hall:
        //   * NO sky background (deep windowless interior) — the env cube is baked
        //     FROM THE SCENE (setIblProbe true), so the club's own neon/UV lights,
        //     blacklight tubes, mirror ball, OLED walls and glowing dance tiles
        //     become the environment every wall/floor/dancer reflects. That is
        //     exactly what a club is: colored bounce off saturated sources.
        //   * ibl 0.40 — enough colored ambient FILL to read the room, low enough to
        //     keep the moody contrast (a club is dim, not flat-lit).
        //   * ambient a low VIOLET floor — the club IS purple, so the deepest corners
        //     lift into club-violet rather than dead gray (unlike the facility, where
        //     a neutral floor was correct).
        //   * iblSpecular 1.3 so the mirror ball, chrome handles and the gleaming
        //     glass bar/countertops catch bright reflections.
        //   * bloom 0.28 (showroom "hero" range) so the HDR emissives + saturated
        //     lights actually BLOOM through the ACES post stack.
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
        device->setIblProbe(true);          // bake the neon room into the env cube
        // GAMMA WALK-BACK (feat/club-gamma-fix, 2026-07-25): the sRGB-encode fix
        // brightened crushed darks the most (~2.4x in the midtones, more in shadow),
        // so the colored ambient FILL that was propping up the black void is now
        // double-counting — the walls lifted into a glowing-purple rave wash. Cut the
        // fill/ambient/bloom back HARD and let the real neon/UV lights + reflections
        // do the work (0.46 -> 0.20 ibl, ambient ~halved, bloom 0.28 -> 0.16).
        device->setIblIntensity(0.20f);     // colored ambient fill — moody, no longer a wash
        device->setIblSpecular(1.30f);      // mirror ball / chrome / glass bar shine (reflections = intent, kept)
        device->setMetalAmbient(1.0f);      // metals keep an F0 response (never black)
        device->setAmbient(0.024f, 0.019f, 0.040f);  // low VIOLET floor (club-purple, not gray) — halved
        device->setExposure(1.0f);
        device->setBloom(0.16f);            // let the neon/blacklight/OLED sing WITHOUT blowing the beams milky

        const x3::phys::Vec3 spawn = club.spawn();

        // ===== Headless screenshot path: pose the showcase camera, settle, grab. =
        if (headless) {
            float cam[5]; club.showcaseCamera(cam);
            // Allow an explicit --shot-cam x,y,z,yaw,pitch override (handy for
            // capturing the caves/boss arena from a custom vantage during verify).
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
            // X3_CLUB_EXPOSURE=<f> (feat/club-npcs): screenshot-only exposure lift so
            // the deliberately-dim Private Lounge NPC shots read (the warm indirect
            // lounge is canon-dark; the live club keeps its 1.0 exposure). Shot path only.
            if (const char* ex = std::getenv("X3_CLUB_EXPOSURE")) device->setExposure((float)std::atof(ex));
            // The CROWD proof needs a longer settle so the dancers desync + drift
            // into readable knots before the capture. An explicit `--screenshot <path>
            // <settle>` count wins (lets a capture land at a chosen beat/phrase so the
            // LASER floor patterns switch + the moving-heads sweep to a specific axis
            // angle across a still SEQUENCE — otherwise every shot froze at frame 24).
            const int kSettle = (hc.screenshotSettle > 24) ? hc.screenshotSettle
                                : ddgiForce ? 120 : (crowdShot ? 150 : 24);
            const float dt = 1.0f / 60.0f;
            // Fallback (headless with no --screenshot, i.e. `--smoketest --world club`):
            // a loose scratch grab in the REPO ROOT, which .gitignore already covers.
            // It used to be an absolute "C:/GameDev/X3Native-engine/..." — a path from
            // before the move to D:, so the write always failed, the host reported
            // "capture FAILED", and `--smoketest --world club` exited 1 on a perfectly
            // healthy club. The smoketest gate could not pass. (Pre-existing; found
            // while running the gates for the OLED pass.)
            const std::string outPath = crowdShot   ? crowdShotPath
                                      : screenshot  ? screenshotPath
                                                    : std::string("agent_club.png");
            // X3_CLUB_SEQ=N: after the settle, capture N CONSECUTIVE frames as
            // <out>_0000.png.. for a motion clip (dancers, beat thump, orb spin) —
            // assembled offline into a GIF/MP4. 0/unset = the single still as before.
            int seqFrames = 0;
            if (const char* sq = std::getenv("X3_CLUB_SEQ")) seqFrames = std::atoi(sq);

            // X3_CLUB_DIALOG=<npc>[:choice] (feat/club-npcs): overlay a canon NPC's
            // chat-tree HUD on the capture — the dialogue-exchange proof shot. Starts
            // <npc>'s `hub` tree; an optional ":N" picks choice N so the shot shows a
            // player answer + the reply. (Headless: no window, drawChatTreeUi runs on
            // the same HUD path the live club loop uses.)
            x3::game::ChatTreeSystem shotChat;
            bool dialogOverlay = false;
            if (const char* dq = std::getenv("X3_CLUB_DIALOG")) {
                shotChat.loadDefault();
                shotChat.ctx().timeline = &x3::game::globalTimeline();
                std::string spec(dq);
                std::string npcId = spec; int pick = -1;
                if (auto c = spec.find(':'); c != std::string::npos) {
                    npcId = spec.substr(0, c); pick = std::atoi(spec.c_str() + c + 1);
                }
                if (shotChat.start(npcId, "hub")) {
                    dialogOverlay = true;
                    if (pick >= 0 && (uint32_t)pick < shotChat.choices().size())
                        shotChat.choose((uint32_t)pick);
                    x3::logInfo("--world club: X3_CLUB_DIALOG overlay — [" +
                                shotChat.currentSpeaker() + "] " + shotChat.currentLine());
                } else {
                    x3::logWarn("--world club: X3_CLUB_DIALOG npc '" + npcId + "' hub failed to start");
                }
            }

            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                club.update(dt, cscene, *device, *cphys);   // ORB spin + spotlight orbit + blacklight pulse + idle props
                clubCrowd.update(dt, cscene);               // the dance-floor crowd
                cphys->step(dt);
                cscene.update(*cphys);
                // Re-pose each frame (scene.update doesn't move the camera).
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                pushLights((float)i * dt, cam[0], cam[1], cam[2]);   // club + nearby Salvari-crystal lights
                if (i == kSettle - 1 && seqFrames == 0) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    cscene.render(*device, frame);
                    club.drawCharacters(*device, frame, cscene);
                    if (dialogOverlay) x3::game::drawChatTreeUi(*device, frame, shotChat);
                }
                device->endFrame(frame);
            }
            bool wrote = true;
            for (int f = 0; f < seqFrames; ++f) {
                char fp[512];
                std::snprintf(fp, sizeof(fp), "%s_%04d.png",
                              outPath.substr(0, outPath.find_last_of('.')).c_str(), f);
                glfwPollEvents();
                club.update(dt, cscene, *device, *cphys);
                clubCrowd.update(dt, cscene);
                cphys->step(dt);
                cscene.update(*cphys);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                pushLights((float)(kSettle + f) * dt, cam[0], cam[1], cam[2]);
                device->armCapture(fp);
                auto frame = device->beginFrame();
                if (frame.valid) {
                    cscene.render(*device, frame);
                    club.drawCharacters(*device, frame, cscene);
                }
                device->endFrame(frame);
                wrote = device->captureFrame(fp) && wrote;
            }
            if (seqFrames == 0) wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world club: wrote screenshot " + outPath);
            else       x3::logError("--world club: capture FAILED");
            cphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: full first-person controller + physics. ===
        // THE MUSIC. The CLUB JUKEBOX (Tim's personal-use "Self Radio") scans
        // assets/audio/club_music/ + the snd_clubmusic_dir user folder for his own
        // MP3/WAVs and streams them through the MUSIC channel; the club beat grid
        // rides each track's BPM (a <track>.json sidecar, else snd_clubmusic_bpm).
        // EMPTY folder -> fall back to the built-in club_descent track exactly as
        // before (~85.5 BPM, house volume, seamless loop) — zero behaviour change.
        // Graceful: no device / missing WAV -> silent club.
        std::unique_ptr<x3::audio::IAudioSystem> caudio(x3::audio::createAudioSystem());
        x3::audio::LoopHandle clubTrack{};
        x3::game::Jukebox jukebox;
        {   // Respect the player's music volume + on/off (Settings). House 0.75
            // default keeps the club audible when the cfg has no music keys yet.
            bool musicOn = true; float musicVol = 0.75f, sfxVol = 1.0f;
            x3::apphost::readAudioSettings(musicOn, musicVol, sfxVol);
            jukebox.rescan(musicVol, musicOn);
            if (caudio && caudio->init()) {
                if (jukebox.hasTracks()) {
                    jukebox.begin(*caudio, club);   // user tracks -> jukebox (retunes the beat grid)
                } else {
                    auto h = caudio->load(x3::game::resolveAudio("music/club_descent.wav"));
                    if (h.valid()) clubTrack = caudio->startLoop(h, 0.75f, 1.0f);
                }
            }
        }
        bool prevNC = false;   // N-key edge for jukebox next/prev
        x3::game::Player cplayer;
        cplayer.spawn(*cphys, spawn.x, spawn.y, spawn.z);

        // ---- CANON DIALOGUE NPCs (feat/club-npcs) — Danny at the U-bar, Amara +
        // Emma in the Private Lounge. Their x3.chattree/1 trees drive E-to-talk.
        // Axis fx (love, etc.) route through the global TimelineState; flag/fire
        // effects ride the runner's own StoryFlags + (absent here) the script sink.
        x3::game::ChatTreeSystem clubChat;
        clubChat.loadDefault();
        clubChat.ctx().timeline = &x3::game::globalTimeline();
        bool prevEc = false;
        bool chatNumPrevC[4] = { false, false, false, false };
        x3::logInfo("--world club: 3 canon NPCs live (Danny @ U-bar, Amara + Emma @ "
                    "Private Lounge) — walk up + E to talk, 1-4 to answer");

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceC = false, prevFC = false;
        bool noclipC = false;
        float flyXc = spawn.x, flyYc = spawn.y + 1.6f, flyZc = spawn.z, flyYawC = 3.14159f, flyPitchC = -0.2f;
        x3::logInfo("--world club: walk THE DEEP at Y=-200 — WASD, mouse look, Space jump, LeftShift sprint, F noclip, Esc to quit");
        if (jukebox.hasTracks())
            x3::logInfo("--world club: JUKEBOX active (" + std::to_string(jukebox.count()) +
                        " track(s)) — N = next, Shift+N = previous");

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

            // JUKEBOX transport: N = next track, Shift+N = previous (edge-triggered).
            bool nNow = kd(GLFW_KEY_N);
            if (nNow && !prevNC && jukebox.hasTracks() && caudio) {
                const bool shift = kd(GLFW_KEY_LEFT_SHIFT) || kd(GLFW_KEY_RIGHT_SHIFT);
                if (shift) jukebox.prev(*caudio, club); else jukebox.next(*caudio, club);
            }
            prevNC = nNow;
            if (caudio) jukebox.update(dt, *caudio, club);   // toast countdown + auto-advance

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
                clubCrowd.update(dt, cscene);               // the dance-floor crowd
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
                clubCrowd.update(dt, cscene);               // the dance-floor crowd
                cphys->step(dt);
                cscene.update(*cphys);
                camX = flyXc; camY = flyYc; camZ = flyZc; camYaw = flyYawC; camPitch = flyPitchC;
            }
            prevSpaceC = spaceNow;

            // ---- CANON NPC DIALOGUE (feat/club-npcs). While a conversation is up,
            // 1-4 answer the filtered choices; E advances a no-choice line. Otherwise
            // E near an NPC (within its talk reach of the eye) starts its entry tree.
            {
                const x3::phys::Vec3 eye{ camX, camY, camZ };
                if (clubChat.active()) {
                    const uint32_t nch = (uint32_t)clubChat.choices().size();
                    for (int ci = 0; ci < 4; ++ci) {
                        const bool dn = kd(GLFW_KEY_1 + ci) || kd(GLFW_KEY_KP_1 + ci);
                        if (dn && !chatNumPrevC[ci] && (uint32_t)ci < nch) {
                            if (clubChat.choose((uint32_t)ci))
                                x3::logInfo("chat: [" + clubChat.currentSpeaker() + "] " +
                                            clubChat.currentLine());
                        }
                        chatNumPrevC[ci] = dn;
                    }
                } else {
                    chatNumPrevC[0] = chatNumPrevC[1] = chatNumPrevC[2] = chatNumPrevC[3] = false;
                }

                const bool eNow = kd(GLFW_KEY_E);
                if (eNow && !prevEc) {
                    if (clubChat.active()) {
                        if (clubChat.choices().empty()) {   // E advances no-choice lines
                            if (clubChat.advance())
                                x3::logInfo("chat: [" + clubChat.currentSpeaker() + "] " +
                                            clubChat.currentLine());
                        }
                    } else {
                        const int who = club.talkTarget(eye);
                        if (who >= 0) {
                            const auto& n = club.canonNpcs()[(size_t)who];
                            if (clubChat.start(n.chatId, n.entryTree))
                                x3::logInfo("chat: talking to " + n.display + " — [" +
                                            clubChat.currentSpeaker() + "] " + clubChat.currentLine());
                        }
                    }
                }
                prevEc = eNow;
            }

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWc || ch != lastHc) { lastWc = cw; lastHc = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
            pushLights((float)now, camX, camY, camZ);   // club + nearby Salvari-crystal lights
            auto frame = device->beginFrame();
            if (frame.valid) {
                cscene.render(*device, frame);
                club.drawCharacters(*device, frame, cscene);
                x3::game::drawChatTreeUi(*device, frame, clubChat);   // NPC dialog HUD
                // JUKEBOX "Now Playing" toast (HUD) — a few seconds after each
                // track change (N/Shift+N or auto-advance). Cyan-tinted mono text,
                // low-left, with a dim backing plate for legibility over the floor.
                if (jukebox.toastRemaining() > 0.0f && !jukebox.toastText().empty()) {
                    const char* txt = jukebox.toastText().c_str();
                    const float px = 22.0f;
                    const float x = 28.0f, y = (float)lastHc - 64.0f;
                    const float w = px * 0.62f * (float)jukebox.toastText().size() + 24.0f;
                    const float plate[4] = { 0.02f, 0.02f, 0.05f, 0.55f };
                    const float ink[4]   = { 0.55f, 0.95f, 1.00f, 1.0f };   // club cyan
                    device->drawHudQuad(frame, x - 12.0f, y - 8.0f, w, px + 18.0f, plate);
                    device->drawHudText(frame, txt, x, y, px, ink);
                }
            }
            device->endFrame(frame);
        }

        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
