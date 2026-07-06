// --world space host — the Act-3 6DOF space-pilot showcase. RE-HOMED from the
// pre-split main() `if (worldMode == "space") { ... }` inline block (feat/
// cockpit-vattalus) into the #28 deep-split world-host registry. The body is the
// VERBATIM space host loop; the ONLY edits are reaching shared state via the
// HostContext (`hc.device` is a raw IRenderDevice*, so the pre-split
// `device.get()`/`device->` become `device`/`device->`), mirroring host_drive.cpp.
#include "world_host_common.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"
#include "../scene.h"
#include "../mesh_prims.h"
#include "../fx.h"
#include "../asset_root.h"
#include "../space_pilot.h"
#include <filesystem>

namespace x3 { namespace apphost {

int hostSpace(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    // ==== VERBATIM host body (re-homed; device is now a raw pointer) ====
    if (worldMode == "space") {
        x3::logInfo("--world space: building the Act-3 space-pilot showcase");
        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world space: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }

        // W3-3 (AD-2 red-line): deep-space STARFIELD. The old host disabled the
        // sky entirely, leaving the flat navy clear color behind the fleet. The
        // analytic sky's procedural starfield is gated to DARK skies and, at
        // haze == 0, paints stars on the FULL sphere (spaceW: a space scene
        // looking "down" sees stars, not a ground plane) — so deep space is the
        // sky ENABLED at near-black with zero haze, exactly like the nightsky
        // host but with no horizon band at all.
        { x3::rhi::IRenderDevice::SkyParams sp{};
          sp.enabled = true;
          sp.sunDir[0] = 0.6f; sp.sunDir[1] = 0.5f; sp.sunDir[2] = 0.62f;   // matches the key light corner
          sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
          sp.sunIntensity = 0.02f;                     // a distant star-disk glint, not daylight
          sp.haze = 0.0f;                              // haze 0 == DEEP SPACE (stars on the full sphere)
          sp.exposure = 1.0f;
          sp.zenith[0]  = 0.003f; sp.zenith[1]  = 0.003f; sp.zenith[2]  = 0.008f;
          sp.horizon[0] = 0.004f; sp.horizon[1] = 0.005f; sp.horizon[2] = 0.011f;
          device->setSkyParams(sp);
          device->setSkyTime(10.0f);                   // non-zero -> starfield twinkle/rotation phase
          // R2: enabling the sky at near-zero sun replaced whatever ambient the
          // disabled-sky path implied — the fleet went silhouette-black. Explicit
          // cool ambient so hulls read while space stays dark (nightsky's trick).
          device->setAmbient(0.11f, 0.12f, 0.16f); }
        // SSAO + SSGI screen-space passes raster the whole scene to black on a
        // black/empty space background (no nearby geometry to bounce off) -- the
        // 1080 Ti / no-RT fallback path documented in the memory bank. Disable
        // both for the space showcase so the ships actually read against the
        // dark backdrop.
        { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false;
          device->setSsaoParams(ap); }
        { x3::rhi::IRenderDevice::GiParams gp{}; gp.enabled = false;
          device->setGiParams(gp); }
        // Sun = the directional sun baked into mesh.frag at +Y-ish; layer on a
        // few BRIGHT point lights NEAR the fleet so the ships read (the analytic
        // sky is OFF -> no atmospheric tint; light only comes from these point
        // lights + the hardcoded sun, attenuated by 1/r^2). The point-light
        // ranges + intensities are intentionally cranked: deep space has zero
        // bounced light, so anything subtle would render the ships as silhouettes.
        // R3: with the REAL black sky in (starfield), the old light rig left the
        // hulls as silhouettes — the navy "readability" of the old shot was just
        // the clear color. Roughly doubled key/fill/rim so the fleet reads as lit
        // metal against the stars.
        { x3::rhi::PointLight pl[3];
          // Key light: a "sun" anchored near the fleet so attenuation is gentle.
          pl[0].pos[0] =  120.0f; pl[0].pos[1] = 120.0f; pl[0].pos[2] = 120.0f;
          pl[0].range  =  600.0f;
          pl[0].color[0] = 130.0f; pl[0].color[1] = 121.0f; pl[0].color[2] = 104.0f;
          // Fill light from -X/+Y to bring out the camera-facing side.
          pl[1].pos[0] = -80.0f; pl[1].pos[1] =  60.0f; pl[1].pos[2] =  20.0f;
          pl[1].range  = 400.0f;
          pl[1].color[0] = 40.0f; pl[1].color[1] = 46.0f; pl[1].color[2] = 60.0f;
          // Rim/back light from +X/-Y to give the ships shape.
          pl[2].pos[0] =  200.0f; pl[2].pos[1] = -30.0f; pl[2].pos[2] = -50.0f;
          pl[2].range  = 500.0f;
          pl[2].color[0] = 24.0f; pl[2].color[1] = 19.0f; pl[2].color[2] = 13.0f;
          device->setPointLights(pl, 3); }

        // ---- Player ship (the SpacePilotController) -----------------------
        x3::game::SpacePilotController pilot;
        pilot.spawn(*sphys, 0.0f, 0.0f, 0.0f);

        // ---- Try to load an actual ship GLB. SpaceShip*.glb don't ship in
        //      assets/rigged_glb yet (per the task brief: "4 SpaceShip*.glb
        //      already in rigged_glb" was aspirational — the dir has none on
        //      this baseline). We fall back to DroneOscillating.glb as a
        //      stand-in flying object; if even that fails, we draw the ship
        //      as a procedural box.
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device, asrc.get()));
        // Probe candidates in order of preference: real ship asset first, drone fallback.
        const char* kShipCandidates[] = {
            "SpaceShip.glb", "SpaceShip2.glb", "SpaceShip3.glb", "SpaceShip4.glb",
            "DroneOscillating.glb", "DroneExportWMotion.glb"
        };
        x3::asset::Model shipModel{};
        std::string shipFile;
        for (const char* c : kShipCandidates) {
            shipModel = mloader->load(c);
            if (shipModel.ok) { shipFile = c; break; }
        }
        std::vector<x3::asset::ModelDrawable> shipDrawables;
        if (shipModel.ok) shipDrawables = x3::asset::makeDrawables(shipModel);
        x3::logInfo(std::string("--world space: ship model=") + (shipModel.ok ? shipFile : "<procedural-box-fallback>"));

        // Procedural-box fallback (in case the GLB load fails entirely).
        x3::prims::PrimMesh sbm = x3::prims::makeBox(2.0f, 0.6f, 1.2f, 0, 0, 0, 0.25f);
        auto shipBoxMesh = device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                              sbm.index.data(), (uint32_t)sbm.index.size());
        auto sbTexD = x3::prims::makeCheckerRGBA(64, 8, 180, 190, 210, 60, 70, 90);
        auto shipBoxTex = device->createTexture(sbTexD.data(), 64, 64, true);

        // ---- Static decor fleet: a wing formation a few dozen meters out
        //      Each is a static placement transform (rotation around +Y for variety).
        //      Coordinates chosen so the headless screenshot camera (at -X behind
        //      the player ship, looking toward +X) sees a tight cluster of ships
        //      filling a good portion of the frame.
        struct DecorShip { float x, y, z, yaw, scale; };
        const DecorShip decor[] = {
            {   30.0f,   2.0f,    8.0f,  0.2f, 18.0f },  // close right
            {   35.0f,   4.0f,  -10.0f, -0.3f, 20.0f },  // close left, up
            {   45.0f,  -2.0f,   18.0f,  0.4f, 22.0f },  // mid-right, down
            {   55.0f,   8.0f,   -4.0f,  0.0f, 24.0f },  // mid lead, up
            {   80.0f,   0.0f,  -25.0f,  0.6f, 28.0f },  // far escort left
            {   80.0f,   2.0f,   25.0f, -0.6f, 28.0f },  // far escort right
        };
        const int kDecorCount = (int)(sizeof(decor) / sizeof(decor[0]));

        // CombatFx for laser tracers + impact decals. Heap-allocated because
        // CombatFx carries ~256 KB of mutable scratch instance arrays; piling
        // another stack copy into main() (which already holds one for the
        // canonplay/level1 paths) overflows the 1 MB default thread stack.
        auto combatFxOwned = std::make_unique<x3::game::CombatFx>();
        x3::game::CombatFx& combatFx = *combatFxOwned;
        combatFx.init(*device);

        const float dt = 1.0f / 60.0f;

        // Draw a ship at a placement matrix (yaw-only for decor; full quat for
        // the player ship). `bright` brightens the model so it reads in the
        // dim space scene.
        auto drawShipAt = [&](const x3::rhi::FrameContext& frame,
                              const float xform[16], float bright) {
            if (shipModel.ok) {
                for (const auto& dr : shipDrawables) {
                    float fin[16];
                    x3::asset::mulMat4(xform, dr.nodeTransform, fin);
                    // Boost the ship brightness HARD: in deep space there is no
                    // bounced light, so the GLB's baseColorFactor (often very
                    // dark scifi metal) reads near-black under direct lighting
                    // alone. The tint multiplier is the renderer's albedo
                    // scale, so we crank it + add a constant floor for the
                    // headless screenshot. The windowed flight feels the same
                    // because the lights are also dialed up to match.
                    float tint[4] = {
                        dr.baseColorFactor[0] * bright * 4.0f + 0.45f,
                        dr.baseColorFactor[1] * bright * 4.0f + 0.50f,
                        dr.baseColorFactor[2] * bright * 4.0f + 0.55f,
                        dr.baseColorFactor[3]
                    };
                    device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                     x3::rhi::TextureHandle{ dr.baseColorTexId },
                                     tint, fin);
                }
            } else {
                const float white[4] = { bright, bright, bright, 1.0f };
                device->drawMesh(frame, shipBoxMesh, shipBoxTex, white, xform);
            }
        };

        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            // Player ship: build a 4x4 from quaternion + position (visible only
            // in 3P; in 1P it would clip the near plane — host gates visuals).
            if (pilot.isThirdPerson()) {
                const x3::phys::Vec3 p = pilot.pos();
                const x3::phys::Vec3 f = pilot.forward();
                const x3::phys::Vec3 r = pilot.right();
                const x3::phys::Vec3 u = pilot.up();
                float m[16] = {
                    f.x, f.y, f.z, 0,   // col 0 = ship local +X
                    u.x, u.y, u.z, 0,   // col 1 = ship local +Y
                    r.x, r.y, r.z, 0,   // col 2 = ship local +Z
                    p.x, p.y, p.z, 1
                };
                drawShipAt(frame, m, 1.5f);
            }
            // Decor fleet.
            for (int i = 0; i < kDecorCount; ++i) {
                const float c = std::cos(decor[i].yaw), s = std::sin(decor[i].yaw);
                const float S = decor[i].scale;
                float m[16] = {
                    c*S, 0,  -s*S, 0,
                    0,   S,  0,   0,
                    s*S, 0,  c*S, 0,
                    decor[i].x, decor[i].y, decor[i].z, 1
                };
                drawShipAt(frame, m, 1.0f);
            }
        };

        // ===== Headless capture (--world space --screenshot <path>) ========
        if (headless) {
            // The frustum-cull pass on this baseline tests AABBs that may be
            // wrong for a deeply-nested GLB drawable transform. Disable for the
            // capture so the test screenshot is robust against that; it is a
            // VISUAL gate, not a perf gate. (Windowed path leaves it default-on.)
            device->setFrustumCullEnabled(false);
            // Camera behind the player ship looking toward +X (yaw=0 -> +X is
            // the device's "forward 0" per Player::camera()), slight downward
            // pitch to catch the slight-Y staggered decor ships. The fleet
            // cluster sits at x=60..200 with +/-Z flanks within FOV.
            float cam[5] = { -25.0f, 6.0f, 0.0f, 0.0f, -0.05f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/space.png");
            // Settle: a few frames so the lights register + the meshes upload.
            const int kFrames = 16;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                sphys->step(dt);
                combatFx.update(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    drawScene(frame);
                    combatFx.submit(*device, frame);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world space: wrote " + outPath);
            else       x3::logError("--world space: capture FAILED");
            combatFx.shutdown(*device);
            device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
            if (shipModel.ok) mloader->unload(shipModel);
            sphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: 6DOF pilot, mouse + WASD + Q/E + V ==
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevV = false, prevLmb = false;
        x3::logInfo("--world space: WASD thrust, mouse look, Q/E roll, Space/Ctrl up/down, Shift boost, V camera, LMB laser, Esc quit");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY); lastMX = mx; lastMY = my;
            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };

            // Build a PlayerInput for the controller. jumpPressed re-purposed as
            // "up impulse this frame" while Space is held; sprint = boost.
            x3::game::PlayerInput in{};
            in.moveFwd    = (kd(GLFW_KEY_W) ?  1.0f : 0.0f) + (kd(GLFW_KEY_S) ? -1.0f : 0.0f);
            in.moveStrafe = (kd(GLFW_KEY_D) ?  1.0f : 0.0f) + (kd(GLFW_KEY_A) ? -1.0f : 0.0f);
            in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed= kd(GLFW_KEY_SPACE);   // held = up impulse
            in.lookDX     = ddx;
            in.lookDY     = ddy;

            // Q/E roll axis: +1 for Q, -1 for E (or the other way; either is fine).
            float rollAxis = (kd(GLFW_KEY_Q) ? -1.0f : 0.0f) + (kd(GLFW_KEY_E) ? 1.0f : 0.0f);
            pilot.setRollInput(rollAxis);

            pilot.update(in, fdt, *sphys);

            // V to toggle 1P / 3P (rising edge).
            bool vNow = kd(GLFW_KEY_V);
            if (vNow && !prevV) pilot.toggleCameraMode();
            prevV = vNow;

            // LMB laser (rising edge -> fire one bolt, log on success).
            bool lmbNow = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmbNow && !prevLmb && pilot.fireLaser(fdt)) {
                // Tracer: from the ship's forward muzzle to a long range hit.
                const x3::phys::Vec3 pos = pilot.pos();
                const x3::phys::Vec3 fwd = pilot.forward();
                x3::phys::Vec3 muzzle{ pos.x + fwd.x * 2.5f,
                                       pos.y + fwd.y * 2.5f,
                                       pos.z + fwd.z * 2.5f };
                x3::phys::Vec3 hit{ pos.x + fwd.x * 400.0f,
                                    pos.y + fwd.y * 400.0f,
                                    pos.z + fwd.z * 400.0f };
                combatFx.addTracer(muzzle, hit);
            }
            prevLmb = lmbNow;

            sphys->step(fdt);
            combatFx.update(fdt);

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0 && chh>0) device->onResize((uint32_t)cw, (uint32_t)chh);

            float cx, cy, cz, cyaw, cpit;
            pilot.camera(cx, cy, cz, cyaw, cpit);
            device->setCamera(cx, cy, cz, cyaw, cpit, 65.0f);

            auto frame = device->beginFrame();
            if (frame.valid) {
                drawScene(frame);
                combatFx.submit(*device, frame);
            }
            device->endFrame(frame);
        }
        combatFx.shutdown(*device);
        device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
        if (shipModel.ok) mloader->unload(shipModel);
        sphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }
    return -1;
}

}} // namespace x3::apphost
