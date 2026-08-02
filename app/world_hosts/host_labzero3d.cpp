// --world labzero3d host — LAB ZERO P0 rail-camera prototype
// (LABZERO_3D_ADDENDUM.md P0, feature/labzero-p0). Skeleton copied from
// hostValley (physics init / job system / worldTerrainConfig streamed terrain /
// analytic SkyParams); valley content + ecology stripped (P0 wants the WORLD,
// not the biome). No GPL / id Tech / RBDOOM source consulted.
//
// P0 scope: a Jolt-capsule character runs left/right along the gameplay rail
// and jumps with placeholder gravity (A1 constants applied naively; the real
// feel layer is P1). Side camera tracks on the rail at FOV 28 with
// critically-damped smoothing + deadzone — NEVER hard-snaps (polish law).
//
// CHARACTER FINDING (P0.2, noted per the addendum): the animated Jake avatar
// EXISTS and is standalone-hostable (app/thirdperson.* + Jake_22_actions.glb,
// same skinning path as the monsters) — but every .glb is LFS-tracked and this
// guest box cannot reach the fleet LFS endpoint (192.168.7.23). So P0 ships
// the spec's contingency capsule (emissive rim + blob shadow); wiring the Jake
// avatar through ThirdPerson IS P1 line-item ZERO on a fleet box.
//
// Controls v2 (RFC): A/D run along the rail; Space OR J jump (edge-press).
// Arrow keys are RESERVED for aim (aiming is never disabled — P1 wires it).
#include "world_host_common.h"
#include "../scene.h"
#include "../terrain.h"
#include "../mesh_prims.h"
#include "../labzero_rail.h"

namespace x3 { namespace apphost {

namespace {

// A1 feel constants, px -> metres via the T2m anchor (105 px apex == 2.207 m,
// so 47.57 px/m; LABZERO_PORT_RFC "SPEC CORRECTED BY EXECUTION"). Applied
// NAIVELY per P0.5 — the verified fixed-step feel layer replaces this in P1.
constexpr float kPxPerM     = 105.0f / 2.207f;      // 47.57
constexpr float kGravity    = 3600.0f / kPxPerM;    // 75.7 m/s^2 (game feel, not Earth)
constexpr float kJumpV      = 900.0f / kPxPerM;     // 18.9 m/s up
constexpr float kRunSpeed   = 300.0f / kPxPerM;     // 6.3 m/s
constexpr float kLateralK   = 4.0f;                 // rail glue gain (1/s), P0.3
constexpr float kFixedDt    = 1.0f / 60.0f;

// Camera spec (P0.4): side offset 14 m, eye lift 1.6 m, look-at char + 0.8 up
// + 2.5 m eased lookahead, FOV 28, halflife 0.12 s horizontal / 0.25 s
// vertical, 1.2 x 0.8 m deadzone box.
constexpr float kCamSide      = 14.0f;
constexpr float kCamUp        = 1.6f;
constexpr float kLookUp       = 0.8f;
constexpr float kLookAhead    = 2.5f;
constexpr float kCamFov       = 28.0f;
constexpr float kHalflifeH    = 0.12f;
constexpr float kHalflifeV    = 0.25f;
constexpr float kDeadW        = 1.2f;   // deadzone width (along-rail, metres)
constexpr float kDeadH        = 0.8f;   // deadzone height (vertical, metres)

// Critically-damped-feel exponential smoothing factor for a given halflife.
inline float smoothAlpha(float dt, float halflife) {
    return 1.0f - std::exp(-dt * 0.6931472f / halflife);
}

} // namespace

int hostLabZero3d(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;

    x3::logInfo("--world labzero3d: LAB ZERO P0 rail slice (LABZERO_3D_ADDENDUM)");

    // ==== World stand-up: hostValley skeleton, valley content stripped. ====
    std::unique_ptr<x3::phys::IPhysicsWorld> lphys(x3::phys::createPhysicsWorld());
    if (!lphys->init()) {
        x3::logError("--world labzero3d: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    std::unique_ptr<x3::jobs::IJobSystem> ljobs(x3::jobs::createJobSystem());
    ljobs->init(0);
    x3::game::Scene lscene;
    const x3::game::TerrainConfig& lcfg = x3::game::worldTerrainConfig();
    {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
    }
    x3::game::TerrainStreamer lstream;
    lstream.init(lscene, *device, *lphys, ljobs.get(), lcfg, 6500.0f, 200.0f, /*radius=*/10);
    // W8-3 horizon ring: the far-terrain stitch that puts the volcanic range
    // in frame (streamed tiles only cover ~256 m; the range is ~1.9 km east).
    {
        x3::game::HorizonRingDesc hr{};
        hr.centerX = 6500.0f; hr.centerZ = 0.0f;   // the rail run
        hr.rInner = 240.0f; hr.rOuter = 13000.0f;
        hr.rings = 140; hr.segments = 160; hr.yBias = -3.0f;
        x3::game::addTerrainHorizonRing(lscene, *device, lstream.groundTexture(), hr);
        device->setCameraFar(15000.0f);
    }

    // ==== The rail: TWO nodes, a straight 400 m run (P0.3). Placement: the
    // volcanic-range foothills (terrain.cpp kRanges: E range along x=9200,
    // z -2000..2500, radius 2300, peaks ~500 m). Running SOUTH along -Z at
    // x=7300 puts the camera (side = up x tangent = -X) 14 m west, looking
    // EAST — the range fills the background ~1.9 km behind the character and
    // FOV 28's compression makes it loom. Node Y rides the terrain height so
    // the camera rail follows the ground profile.
    x3::game::LabZeroRail rail;
    {
        // One-time placement probe (P0 verify-first): the eastward height
        // profile from the rail toward the volcanic ridge, so the G4 framing
        // is placed by DATA, not by eye (X3_WORLD_RULES rule 0).
        char probe[160];
        for (float px = 3000.0f; px <= 9400.0f; px += 400.0f) {
            std::snprintf(probe, sizeof probe, "[labzero3d] probe h(%5.0f, 0) = %7.2f",
                          px, x3::game::terrainHeightAtWorld(px, 0.0f));
            x3::logInfo(probe);
        }
        std::vector<x3::game::RailNode> nodes;
        nodes.push_back({ 6500.0f, x3::game::terrainHeightAtWorld(6500.0f,  200.0f),  200.0f });
        nodes.push_back({ 6500.0f, x3::game::terrainHeightAtWorld(6500.0f, -200.0f), -200.0f });
        rail.init(nodes);
    }

    // ==== Character: the spec capsule (see CHARACTER FINDING above). ====
    const x3::game::RailNode start = rail.pos(0.0f);
    const x3::phys::Vec3 spawn{ start.x, start.y + 2.0f, start.z };
    const x3::phys::BodyId charBody = lphys->createCharacter(0.35f, 1.85f, spawn);

    // Visual: emissive-rim capsule proxy (cylinder) + a dark blob-shadow disc.
    uint32_t capsuleEnt = 0, shadowEnt = 0;
    {
        x3::prims::PrimMesh cap = x3::prims::makeCylinder(0.35f, 0.35f, 0.925f, 24);
        x3::game::Entity e;
        e.mesh = device->createMesh(cap.verts.data(), (uint32_t)cap.verts.size(),
                                    cap.index.data(), (uint32_t)cap.index.size());
        // Suit blue + a modest emissive rim (flat emissive stays under the ACES
        // clip threshold — X3_WORLD_RULES rule 5).
        e.baseColor[0] = 0.16f; e.baseColor[1] = 0.35f; e.baseColor[2] = 0.55f; e.baseColor[3] = 1.0f;
        e.emissive[0] = 0.10f; e.emissive[1] = 0.35f; e.emissive[2] = 0.45f; e.emissive[3] = 0.45f;
        e.tag = (uint32_t)x3::game::Tag::Prop;
        capsuleEnt = lscene.add(e);

        x3::prims::PrimMesh blob = x3::prims::makeCylinder(0.5f, 0.5f, 0.02f, 20);
        x3::game::Entity s;
        s.mesh = device->createMesh(blob.verts.data(), (uint32_t)blob.verts.size(),
                                    blob.index.data(), (uint32_t)blob.index.size());
        s.baseColor[0] = 0.02f; s.baseColor[1] = 0.02f; s.baseColor[2] = 0.03f; s.baseColor[3] = 1.0f;
        s.tag = (uint32_t)x3::game::Tag::Prop;
        shadowEnt = lscene.add(s);
    }
    auto placeVisuals = [&](const x3::phys::Vec3& feet) {
        // Capsule visual centered on the body; blob shadow hugs the ground.
        x3::game::Entity& c = lscene.get(capsuleEnt);
        c.transform[12] = feet.x; c.transform[13] = feet.y + 0.925f; c.transform[14] = feet.z;
        const float gy = x3::game::terrainHeightAtWorld(feet.x, feet.z);
        x3::game::Entity& s = lscene.get(shadowEnt);
        s.transform[12] = feet.x; s.transform[13] = gy + 0.03f; s.transform[14] = feet.z;
    };

    // ==== Sim state ====
    float s = 0.0f;              // arc-length param tracking the character
    float vy = 0.0f;             // vertical velocity (placeholder feel)
    float facing = 1.0f;         // eased facing sign for the camera lookahead
    bool  prevJumpKey = false;
    // Camera smoothed state (seeded on first use).
    bool  camSeeded = false;
    float camX = 0, camY = 0, camZ = 0;
    float lookX = 0, lookY = 0, lookZ = 0;

    auto stepSim = [&](float inMove, bool jumpHeld, float dt) {
        const bool grounded = lphys->characterGrounded(charBody);
        const bool jumpEdge = jumpHeld && !prevJumpKey;   // edge-press only (B3)
        prevJumpKey = jumpHeld;
        if (grounded && vy < 0.0f) vy = 0.0f;
        if (jumpEdge && grounded) vy = kJumpV;
        if (!grounded) vy -= kGravity * dt;

        const x3::phys::Vec3 feet = lphys->getBodyPosition(charBody);
        s = rail.closestParam(feet.x, feet.z, s);
        const x3::game::RailNode t = rail.tangent(s);
        const x3::game::RailNode rp = rail.pos(s);
        // Lateral glue: horizontal offset back to the rail, ⊥ to the tangent.
        float offX = rp.x - feet.x, offZ = rp.z - feet.z;
        const float along = offX * t.x + offZ * t.z;
        offX -= along * t.x; offZ -= along * t.z;
        const x3::phys::Vec3 vel{ t.x * (inMove * kRunSpeed) + offX * kLateralK,
                                  vy,
                                  t.z * (inMove * kRunSpeed) + offZ * kLateralK };
        lphys->moveCharacter(charBody, vel, dt);
        if (inMove != 0.0f)
            facing += (((inMove > 0.0f) ? 1.0f : -1.0f) - facing) * smoothAlpha(dt, 0.15f);
    };

    auto updateCamera = [&](float dt) {
        const x3::phys::Vec3 feet = lphys->getBodyPosition(charBody);
        const x3::game::RailNode t  = rail.tangent(s);
        const x3::game::RailNode rp = rail.pos(s);
        // side = normalize(cross(up, tangent)) — the rail's screen-side axis.
        float sideX = -t.z, sideZ = t.x;
        const float sl = std::sqrt(sideX * sideX + sideZ * sideZ);
        if (sl > 1e-4f) { sideX /= sl; sideZ /= sl; }
        // Ideal camera + look targets per the spec.
        const float idealCamX = rp.x + sideX * kCamSide;
        const float idealCamY = rp.y + kCamUp;
        const float idealCamZ = rp.z + sideZ * kCamSide;
        const float idealLookX = feet.x + t.x * (facing * kLookAhead);
        const float idealLookY = feet.y + kLookUp;
        const float idealLookZ = feet.z + t.z * (facing * kLookAhead);
        if (!camSeeded) {
            camX = idealCamX; camY = idealCamY; camZ = idealCamZ;
            lookX = idealLookX; lookY = idealLookY; lookZ = idealLookZ;
            camSeeded = true;
        }
        // Deadzone on the LOOK target: along-rail + vertical components move
        // only once the ideal escapes the 1.2 x 0.8 box (then ease — no snap).
        {
            float dAlong = (idealLookX - lookX) * t.x + (idealLookZ - lookZ) * t.z;
            const float dV = idealLookY - lookY;
            float moveAlong = 0.0f, moveV = 0.0f;
            if (dAlong >  kDeadW * 0.5f) moveAlong = dAlong - kDeadW * 0.5f;
            if (dAlong < -kDeadW * 0.5f) moveAlong = dAlong + kDeadW * 0.5f;
            if (dV >  kDeadH * 0.5f) moveV = dV - kDeadH * 0.5f;
            if (dV < -kDeadH * 0.5f) moveV = dV + kDeadH * 0.5f;
            const float aH = smoothAlpha(dt, kHalflifeH), aV = smoothAlpha(dt, kHalflifeV);
            lookX += t.x * moveAlong * aH;
            lookZ += t.z * moveAlong * aH;
            lookY += moveV * aV;
        }
        const float aH = smoothAlpha(dt, kHalflifeH), aV = smoothAlpha(dt, kHalflifeV);
        camX += (idealCamX - camX) * aH;
        camZ += (idealCamZ - camZ) * aH;
        camY += (idealCamY - camY) * aV;
        // Terrain clearance: never let the rail camera sink into a rise between
        // it and the character (found by the G4 shot at the range wall — the 2D
        // game could not have this bug class; the 3D one just did).
        const float camGround = x3::game::terrainHeightAtWorld(camX, camZ);
        if (camY < camGround + 1.2f) camY = camGround + 1.2f;
        // Camera basis: fwd = (cos p cos y, sin p, cos p sin y) (device law).
        float fx = lookX - camX, fy = lookY - camY, fz = lookZ - camZ;
        const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl > 1e-4f) { fx /= fl; fy /= fl; fz /= fl; }
        const float yaw = std::atan2(fz, fx);
        const float pitch = std::asin(std::max(-1.0f, std::min(1.0f, fy)));
        device->setCamera(camX, camY, camZ, yaw, pitch, kCamFov);
    };

    // ===== Headless: settle the streaming ring, run the sim, grab the G4 shot. =====
    if (headless) {
        const std::string outPath = screenshot && !screenshotPath.empty()
                                        ? screenshotPath
                                        : std::string("labzero_p0_rail.png");
        lstream.setUploadBudget(64);
        const int kSettle = 120;
        for (int i = 0; i < kSettle; ++i) {
            // Auto-run right for the back half so the shot catches motion on the rail.
            const float inMove = (i > kSettle / 2) ? 1.0f : 0.0f;
            stepSim(inMove, false, kFixedDt);
            const x3::phys::Vec3 feet = lphys->getBodyPosition(charBody);
            lstream.update(lscene, *device, *lphys, feet.x, feet.z);
            lphys->step(kFixedDt);
            lscene.update(*lphys);
            placeVisuals(lphys->getBodyPosition(charBody));
            updateCamera(kFixedDt);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) lscene.render(*device, frame);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world labzero3d: wrote screenshot " + outPath);
        else       x3::logError("--world labzero3d: capture FAILED");
        lstream.shutdown(lscene, *device, *lphys);
        lphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Windowed: fixed-timestep sim loop + rail camera. =====
    x3::logInfo("--world labzero3d: A/D run, Space or J jump (arrows reserved for aim), Esc quit");
    double prevTime = glfwGetTime();
    float acc = 0.0f;
    int lastW = (int)W, lastH = (int)H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        double now = glfwGetTime();
        float frameDt = (float)(now - prevTime); prevTime = now;
        if (frameDt > 0.1f) frameDt = 0.1f;

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        float inMove = 0.0f;
        if (kd(GLFW_KEY_D)) inMove += 1.0f;
        if (kd(GLFW_KEY_A)) inMove -= 1.0f;
        const bool jumpHeld = kd(GLFW_KEY_SPACE) || kd(GLFW_KEY_J);

        // Fixed-step sim (the P1 feel layer needs determinism; P0 matches it).
        acc += frameDt;
        while (acc >= kFixedDt) {
            stepSim(inMove, jumpHeld, kFixedDt);
            lphys->step(kFixedDt);
            acc -= kFixedDt;
        }

        const x3::phys::Vec3 feet = lphys->getBodyPosition(charBody);
        lstream.update(lscene, *device, *lphys, feet.x, feet.z);
        lscene.update(*lphys);
        placeVisuals(feet);
        updateCamera(frameDt);

        int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
        if (cw != lastW || chh != lastH) { lastW = cw; lastH = chh; if (cw > 0 && chh > 0) device->onResize((uint32_t)cw, (uint32_t)chh); }

        auto frame = device->beginFrame();
        if (frame.valid) lscene.render(*device, frame);
        device->endFrame(frame);
    }

    lstream.shutdown(lscene, *device, *lphys);
    lphys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
