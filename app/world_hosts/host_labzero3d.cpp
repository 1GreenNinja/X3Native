// ============================================================================
// --world labzero3d — LABZERO_3D_ADDENDUM.md slice P0: RAIL CAMERA PROTOTYPE.
//
// "A 3D side-scroller with cave transitions is NOT two games." (Addendum A0.)
// This host is the proof: ONE streamed world (the same terrain + analytic sky
// the valley host builds), ONE Jolt character, ONE gameplay spline, and ONE
// camera rig hung off that spline. P2's cave seam blends the rail weight to 0
// and the world just opens up — nothing here is a side-scroller "mode".
//
// Setup is lifted from host_valley.cpp per the addendum's instruction ("copy
// hostValley's setup VERBATIM — that ~60 lines buys the entire world"); the
// valley's ecology/water/NPC content is deliberately NOT carried over (P0 wants
// the rail readable and the 1060 3 GB budget honest).
//
// ---------------------------------------------------------------------------
// TWO FINDINGS THIS SLICE OWES THE ADDENDUM. The addendum's own rule is "if you
// change the plan, change THIS file in the same commit" — but LABZERO_3D_ADDENDUM.md
// lives in the EscapeLabZero repo, not this one, so the findings are recorded
// HERE and must be carried across when the two repos are reconciled. Whoever
// does that: F1 changes P1's scope and F2 answers P0 §2's open question.
//
// F1 — GRAVITY CANNOT BE A1 IN P0.  A1 specifies GRAVITY 75.7 m/s^2 (~7.7 g,
//   "this is WHY it feels snappy; do not fix to 9.81"). The physics seam does
//   not allow it from the app layer: IPhysicsWorld::moveCharacter records only
//   the HORIZONTAL desired velocity, treats .y > 0 as a one-shot jump impulse
//   and IGNORES .y <= 0, and the Jolt world integrates gravity internally at
//   -9.81 (JoltPhysicsWorld::init -> SetGravity; the seam is documented at the
//   top of app/player.cpp). P0 may not touch engine/ (addendum GATES), so this
//   host matches the A1 APEX exactly — jumpVelocity = sqrt(2 * 9.81 * 2.207 m)
//   — and accepts the longer air time (~40 steps vs A1's 29). The jump lands
//   where the spec says; it hangs there longer. P1 owns the fix, and there are
//   two honest routes: (a) per-body gravity scale on the character (an engine
//   change, so it belongs to whoever owns engine/), or (b) setCharacterSwim(),
//   which by contract takes desiredVelocity VERBATIM in all three components
//   with NO internal gravity — i.e. the app layer could integrate 75.7 itself.
//   (b) needs the grounded-flag semantics under swim mode verified first,
//   which is P1 work, not a P0 guess.
//
// F2 — THE AVATAR IS THE REAL ONE WHEN THE ASSETS ARE THERE.  Addendum P0 §2
//   asks whether the existing player character can be hosted standalone: it
//   can. ThirdPersonView (app/thirdperson.h) needs only build(scene, device,
//   modelDir) + update(...) + drawAvatar(...) — no canon-world entanglement —
//   so this host drives the REAL skinned Jake with real locomotion blending,
//   which is the no-slop answer. It degrades by design: on a machine whose
//   rigged GLBs are unavailable, build() leaves it unbuilt and we fall back to
//   a lit capsule proxy so the rail is still verifiable. (On the 7700K dev box
//   the GLBs are git-lfs pointer files against an unreachable LFS server, so
//   the proxy is what renders there — that is an asset-availability condition,
//   not a code path missing.)
// ============================================================================

#include "world_host_common.h"
#include "../scene.h"
#include "../terrain.h"
#include "../thirdperson.h"
#include "../mesh_prims.h"
#include "../asset_root.h"
#include "../labzero/labzero_rail.h"

namespace x3 { namespace apphost {

namespace {

using x3::game::labzero::Rail;

// ---- A1 UNITS TABLE (LABZERO_3D_ADDENDUM.md §A1) ---------------------------
// Every number here is a C# px value multiplied by PX2M = 0.021023. The FEEL is
// preserved because the RATIOS are preserved — do not "round these to nicer
// metres", the ratios are the design.
constexpr float kMoveSpeed   = 6.31f;    // MOVE_SPEED 300 px/s
constexpr float kRunMult     = 1.7f;     // run multiplier -> 10.72 m/s
constexpr float kJumpApex    = 2.207f;   // 105 px EXACT (spec corrected by execution)
constexpr float kWorldGravity= 9.81f;    // what the Jolt world actually integrates (F1)
constexpr float kCapRadius   = 0.35f;    // capsule radius
constexpr float kCapHeight   = 1.85f;    // Jake, 88 px
constexpr float kEyeHeight   = 1.6f;     // avatar drive only (no FP camera on the rail)

// ---- Rail + camera rig (addendum P0 §3/§4) ---------------------------------
constexpr float kRailLength  = 400.0f;   // P0 ships a straight 400 m run
// Rail anchor: the WEST crystal-hills flank (ridge x = -8600, app/terrain.cpp
// kRanges[3]). See the placement note at the rail build for why 1300 m out.
// 2100 m east of the ridge, not 1300: at 1300 m the 320 m peaks subtend ~14 deg
// and, standing on the rising flank, the ridge crest sat ABOVE the top of a
// 28 deg frame — mountains you cannot see are not a backdrop. At this distance
// the crest lands in the upper third with sky above it.
constexpr float kRailX       = -6500.0f;
constexpr float kRailZ0      = -kRailLength * 0.5f;
constexpr float kLateralGain = 4.0f;     // k ~= 4/s glue back to the rail
constexpr float kCamSide     = 14.0f;    // m perpendicular offset
constexpr float kCamUp       = 1.6f;     // m above the rail
// Rig lift above the rail. Keep this SMALL: the camera sits 14 m out, so every
// metre of lift is ~4 deg of downward pitch, and past ~2 m the 28 deg lens is
// looking at nothing but ground. The mountains read because the lens is level,
// not because the camera is high.
// ZERO. The arithmetic is unforgiving with a 28 deg lens: the camera sits 14 m
// out, so lift L pitches the view down by atan(L/14) and the frame spans only
// +-14 deg about that. The ridge crest 2.1 km west subtends +8.7 deg, so even a
// 1 m lift (-4 deg) pushes it past the top edge and the shot is all grass. Level
// lens, look target at the same height as the eye: horizon centred, ridge in the
// upper third, sky above it.
constexpr float kCamLift     = 0.0f;
constexpr float kCamFov      = 28.0f;    // compressed, painterly
constexpr float kLookAhead   = 2.5f;     // m of eased lead in the travel direction
constexpr float kLookUp      = 1.6f;     // m above the feet — level with the eye (see kCamLift)
constexpr float kHalfLifeH   = 0.12f;    // s — horizontal critical damping
constexpr float kHalfLifeV   = 0.25f;    // s — vertical (slower: hills must not bob)
constexpr float kDeadHalfW   = 0.6f;     // 1.2 m wide deadzone box
constexpr float kDeadHalfH   = 0.4f;     // 0.8 m tall

// ---- Fixed step (A1: FIXED_TIMESTEP / MAX_STEPS unchanged from the C#) -----
constexpr float    kFixedStep = 1.0f / 60.0f;
constexpr int      kMaxSteps  = 5;

// Critically-damped smoothing factor for a given half-life. Frame-rate
// independent: the same halflife converges identically at 30 or 300 fps, which
// is the whole reason the polish law says "never hard-snap".
inline float damp01(float halfLife, float dt) {
    if (halfLife <= 1e-5f) return 1.0f;
    return 1.0f - std::exp2(-dt / halfLife);
}

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

} // namespace

int hostLabZero3D(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;

    x3::logInfo("--world labzero3d: LAB ZERO P0 — rail-camera prototype "
                "(3D side-scroller on the streamed mountain world)");

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("--world labzero3d: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ---- The world: streamed terrain + analytic sky (hostValley setup) -----
    std::unique_ptr<x3::jobs::IJobSystem> jobs(x3::jobs::createJobSystem());
    jobs->init(0);
    x3::game::Scene scene;
    const x3::game::TerrainConfig& cfg = x3::game::worldTerrainConfig();
    {
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
    }
    // Horizon: the rail's whole point is that the mountains it skirts are REAL
    // geometry receding into depth, so the far plane has to reach them.
    device->setCameraFar(15000.0f);

    x3::game::TerrainStreamer stream;
    stream.init(scene, *device, *phys, jobs.get(), cfg, kRailX, kRailZ0, /*radius=*/8);

    // The streamed ring is only ~8 tiles wide; the ranges this rail is aimed at
    // are kilometres away, so without the far-terrain stitch the backdrop is an
    // empty horizon. The ring samples the SAME procedural field the streamer
    // does, so the peaks on the skyline and the ground underfoot agree by
    // construction (app/terrain.h W8-3).
    {
        x3::game::HorizonRingDesc hr{};
        hr.centerX = kRailX; hr.centerZ = kRailZ0 + kRailLength * 0.5f;
        hr.rInner = 240.0f; hr.rOuter = 13000.0f;
        hr.rings = 140; hr.segments = 160; hr.yBias = -3.0f;
        x3::game::addTerrainHorizonRing(scene, *device, stream.groundTexture(), hr);
    }

    // ---- The rail (P0 §3): a straight 400 m run that rides the heightfield --
    Rail rail;
    rail.setGroundFn([](float x, float z) {
        return x3::game::terrainHeightAtWorld(x, z);
    });
    // WHERE the rail lives matters as much as its shape. The world origin is the
    // facility plain — dead flat by design; the four ranges sit 7-10 km out
    // (app/terrain.cpp kRanges). A rail on the plain satisfies "runs and jumps"
    // and fails "visibly skirts mountains", so P0 runs the flank of the WEST
    // crystal hills: the ridge line is x = -8600 (~320 m peaks, relief falling
    // off over ~2400 m), and the rail is laid 1300 m east of it — far enough to
    // stand on real sloping foothills, close enough that the ridge fills the
    // backdrop. The rail runs along Z, so side() = +X: the camera hangs east and
    // looks WEST, putting the mountains behind the character. That is the
    // side-scroller composition, and it comes from the geometry, not a skybox.
    const float spawnY = x3::game::terrainHeightAtWorld(kRailX, kRailZ0);
    rail.setStraight(x3::phys::Vec3{ kRailX, spawnY, kRailZ0 }, 0.0f, 1.0f, kRailLength);
    if (!rail.valid()) {
        x3::logError("--world labzero3d: rail failed to build");
        stream.shutdown(scene, *device, *phys);
        phys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Start a little way in so the camera has rail behind it on frame one.
    float railS = 8.0f;
    const x3::phys::Vec3 startPt = rail.point(railS);
    const x3::phys::BodyId charId =
        phys->createCharacter(kCapRadius, kCapHeight,
                              x3::phys::Vec3{ startPt.x, startPt.y + 0.5f, startPt.z });

    // ---- The avatar (F2): the real skinned Jake when his GLBs are present ---
    x3::game::ThirdPersonView avatar;
    avatar.build(scene, *device, x3::game::riggedGlbRoot());
    avatar.setThirdPerson(true);
    const bool haveAvatar = avatar.built();

    // Capsule proxy — only stood up when the avatar could not load, so a machine
    // without the rigged GLBs still shows a readable character on the rail.
    uint32_t proxyId = 0xFFFFFFFFu;
    if (!haveAvatar) {
        x3::logWarn("--world labzero3d: rigged avatar unavailable — capsule proxy "
                    "(asset condition, see F2 in this file)");
        x3::prims::PrimMesh m =
            x3::prims::makeCylinder(kCapRadius, kCapRadius, kCapHeight * 0.5f, 20u, 1.0f);
        x3::game::Entity e;
        e.mesh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                    m.index.data(), (uint32_t)m.index.size());
        e.baseColor[0] = 0.55f; e.baseColor[1] = 0.62f; e.baseColor[2] = 0.72f;
        e.baseColor[3] = 1.0f;
        // Emissive RIM so the silhouette reads against a backlit mountain — the
        // addendum's own words for the proxy ("capsule + emissive rim material").
        e.emissive[0] = 0.16f; e.emissive[1] = 0.42f; e.emissive[2] = 0.75f;
        e.emissive[3] = 1.2f;
        e.tag = (uint32_t)x3::game::Tag::Prop;
        proxyId = scene.add(e);
    }

    // ---- Camera rig state (smoothed; never snapped) ------------------------
    x3::phys::Vec3 charPos = startPt;
    float camX = 0, camY = 0, camZ = 0;          // smoothed camera position
    float lookX = 0, lookY = 0, lookZ = 0;       // smoothed look target
    float facing = 1.0f;                          // +1 travelling +s, -1 travelling -s
    float easedLead = 0.0f;                       // eased look-ahead scalar
    bool  rigPrimed = false;

    // Advance ONE fixed step of gameplay. Returns the desired velocity actually
    // issued (diagnostics / future test hooks).
    auto stepGameplay = [&](float moveAxis, bool run, bool jumpEdge) {
        charPos = phys->getBodyPosition(charId);
        railS = rail.nearestS(charPos);

        const x3::phys::Vec3 tan = rail.tangent(railS);
        const x3::phys::Vec3 sd  = rail.side(railS);

        // Lateral correction: pull the capsule back onto the line rather than
        // hard-constraining it, so Jolt keeps resolving collisions normally.
        const float lat = rail.lateralOffset(railS, charPos);
        const float corr = -lat * kLateralGain;

        const float speed = kMoveSpeed * (run ? kRunMult : 1.0f);
        x3::phys::Vec3 desired{
            tan.x * moveAxis * speed + sd.x * corr,
            0.0f,
            tan.z * moveAxis * speed + sd.z * corr
        };

        // F1: .y > 0 is a one-shot jump impulse; gravity is the world's.
        if (jumpEdge && phys->characterGrounded(charId))
            desired.y = std::sqrt(2.0f * kWorldGravity * kJumpApex);

        phys->moveCharacter(charId, desired, kFixedStep);
        if (moveAxis > 0.01f)      facing =  1.0f;
        else if (moveAxis < -0.01f) facing = -1.0f;
        return desired;
    };

    // Place the camera for the current character/rail state. `dt` drives the
    // damping; pass a huge dt (or rigPrimed=false) to prime it on frame one.
    auto updateCamera = [&](float dt) {
        const x3::phys::Vec3 rp = rail.point(railS);
        const x3::phys::Vec3 sd = rail.side(railS);
        const x3::phys::Vec3 tn = rail.tangent(railS);

        // The rig sits ABOVE the rail, not on it. At eye level on a hillside the
        // near slope eats the frame and the ridge behind the character is lost;
        // lifting the camera and looking very slightly down puts the horizon in
        // the upper third, which is what makes the mountains read as backdrop.
        const float tgtCamX = rp.x + sd.x * kCamSide;
        const float tgtCamY = rp.y + kCamUp + kCamLift;
        const float tgtCamZ = rp.z + sd.z * kCamSide;

        // Look target: the character, plus an EASED lead in the travel direction
        // (easing the lead — not snapping it — is what stops the frame lurching
        // when the player reverses).
        easedLead = lerpf(easedLead, facing * kLookAhead, damp01(0.18f, dt));
        const float rawLookX = charPos.x + tn.x * easedLead;
        const float rawLookY = charPos.y + kLookUp;
        const float rawLookZ = charPos.z + tn.z * easedLead;

        if (!rigPrimed) {
            camX = tgtCamX; camY = tgtCamY; camZ = tgtCamZ;
            lookX = rawLookX; lookY = rawLookY; lookZ = rawLookZ;
            rigPrimed = true;
        } else {
            const float aH = damp01(kHalfLifeH, dt);
            const float aV = damp01(kHalfLifeV, dt);
            camX = lerpf(camX, tgtCamX, aH);
            camZ = lerpf(camZ, tgtCamZ, aH);
            camY = lerpf(camY, tgtCamY, aV);

            // Deadzone box: inside it the look target does not move at all, so
            // small hops and footstep jitter never push the frame around.
            const float dxl = rawLookX - lookX, dyl = rawLookY - lookY, dzl = rawLookZ - lookZ;
            const float planar = std::sqrt(dxl * dxl + dzl * dzl);
            if (planar > kDeadHalfW) {
                const float pull = (planar - kDeadHalfW) / planar;
                lookX = lerpf(lookX, lookX + dxl * pull, aH);
                lookZ = lerpf(lookZ, lookZ + dzl * pull, aH);
            }
            if (std::fabs(dyl) > kDeadHalfH) {
                const float over = dyl - (dyl > 0 ? kDeadHalfH : -kDeadHalfH);
                lookY = lerpf(lookY, lookY + over, aV);
            }
        }

        float fx = lookX - camX, fy = lookY - camY, fz = lookZ - camZ;
        const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl > 1e-4f) { fx /= fl; fy /= fl; fz /= fl; }
        const float yaw   = std::atan2(fz, fx);
        const float pitch = std::asin(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy));
        device->setCamera(camX, camY, camZ, yaw, pitch, kCamFov);
    };

    // Push the character's transform into whichever visual represents it.
    auto updateVisual = [&](float dt) {
        const x3::phys::Vec3 feet = charPos;
        if (haveAvatar) {
            const float moveYaw = (facing >= 0.0f) ? 0.0f : 3.14159265f;  // rail is +X
            avatar.update(dt, scene, feet, kEyeHeight, moveYaw, 0.0f,
                          x3::game::kNoRoom, /*crouched=*/false, /*fireHeld=*/false);
        } else if (proxyId != 0xFFFFFFFFu) {
            x3::game::Entity& e = scene.get(proxyId);
            e.transform[12] = feet.x;
            e.transform[13] = feet.y + kCapHeight * 0.5f;
            e.transform[14] = feet.z;
        }
    };

    // ===== Headless: settle the stream ring, then grab the G4 proof shot =====
    if (headless) {
        const std::string outPath = screenshot ? screenshotPath
                                               : std::string("shots/labzero_p0/p0_rail.png");
        stream.setUploadBudget(64);
        const int kSettle = 96;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            // Walk right the whole settle so the capture catches real locomotion
            // on the rail rather than a T-pose at the spawn point.
            // Jump early so the capture frame shows GROUND CONTACT, not a pose
            // frozen mid-arc: with the F1 apex-matched impulse the air time is
            // ~80 steps, so a press at step 10 has landed well before frame 95.
            stepGameplay(/*moveAxis=*/1.0f, /*run=*/false, /*jumpEdge=*/(i == 10));
            phys->step(kFixedStep);
            scene.update(*phys);
            stream.update(scene, *device, *phys, charPos.x, charPos.z);
            updateVisual(kFixedStep);
            updateCamera(kFixedStep);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                if (haveAvatar) avatar.drawAvatar(*device, frame, scene);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world labzero3d: wrote screenshot " + outPath);
        else       x3::logError("--world labzero3d: capture FAILED");
        stream.shutdown(scene, *device, *phys);
        phys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Windowed: controls v2 on the rail =================================
    x3::logInfo("--world labzero3d: A/D or Left/Right run the rail, Space or J jump, "
                "LeftShift run, Esc quit");
    double prevTime = glfwGetTime();
    float  accumulator = 0.0f;
    bool   prevJump = false;
    int    lastW = (int)W, lastH = (int)H;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        const double now = glfwGetTime();
        float dt = (float)(now - prevTime);
        prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        float moveAxis = 0.0f;
        if (kd(GLFW_KEY_D) || kd(GLFW_KEY_RIGHT)) moveAxis += 1.0f;
        if (kd(GLFW_KEY_A) || kd(GLFW_KEY_LEFT))  moveAxis -= 1.0f;
        const bool run = kd(GLFW_KEY_LEFT_SHIFT);
        // Controls v2: jumpHeld = Space || J. Edge-press only (B3 lives here).
        const bool jumpNow  = kd(GLFW_KEY_SPACE) || kd(GLFW_KEY_J);
        bool       jumpEdge = jumpNow && !prevJump;
        prevJump = jumpNow;

        // Fixed-step accumulator, MAX_STEPS clamp (A1). Dragging the window must
        // not slow the game down — that is the accumulator's whole point.
        accumulator += dt;
        int steps = 0;
        while (accumulator >= kFixedStep && steps < kMaxSteps) {
            stepGameplay(moveAxis, run, jumpEdge);
            jumpEdge = false;          // one impulse per press, never per step
            phys->step(kFixedStep);
            accumulator -= kFixedStep;
            ++steps;
        }
        if (accumulator > kFixedStep * (float)kMaxSteps) accumulator = 0.0f;

        scene.update(*phys);
        charPos = phys->getBodyPosition(charId);
        stream.update(scene, *device, *phys, charPos.x, charPos.z);
        updateVisual(dt);
        updateCamera(dt);

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) {
            lastW = cw; lastH = ch;
            if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch);
        }

        auto frame = device->beginFrame();
        if (frame.valid) {
            scene.render(*device, frame);
            if (haveAvatar) avatar.drawAvatar(*device, frame, scene);
        }
        device->endFrame(frame);
    }

    stream.shutdown(scene, *device, *phys);
    phys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
