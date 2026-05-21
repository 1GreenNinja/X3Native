// X3Engine host — opens a window, brings up the render device + physics, builds
// the S2 graybox test level, and runs the loop with a fly camera. Walking is S3.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/IConsole.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IAssetSource.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"

#include "mesh_prims.h"
#include "scene.h"
#include "level.h"
#include "player.h"
#include "door.h"
#include "weapon.h"

#include <memory>
#include <string_view>
#include <string>
#include <cmath>
#include <vector>
#include <cstdint>

int main(int argc, char** argv) {
    bool smoketest = false, testAsset = false, testConsole = false, testPhysics = false,
         testGltf = false, testPlayer = false, testInteract = false, testPickup = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--smoketest") smoketest = true;
        else if (a == "--test-asset") testAsset = true;
        else if (a == "--test-console") testConsole = true;
        else if (a == "--test-physics") testPhysics = true;
        else if (a == "--test-gltf") testGltf = true;
        else if (a == "--test-player") testPlayer = true;
        else if (a == "--test-interact") testInteract = true;
        else if (a == "--test-pickup") testPickup = true;
    }

    // Headless self-tests (no window / Vulkan needed)
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
    if (testPickup) {
        x3::logInfo("running weapon pickup + arming (S5) self-test...");
        return x3::game::runPickupSelfTest() ? 0 : 1;
    }

    x3::logInfo("X3Engine starting...");

    if (!glfwInit()) {
        x3::logError("glfwInit failed");
        return 1;
    }
    // No OpenGL/GLES context — we drive Vulkan ourselves.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    const uint32_t W = 1280, H = 720;
    GLFWwindow* window = glfwCreateWindow(static_cast<int>(W), static_cast<int>(H),
                                          "X3Engine", nullptr, nullptr);
    if (!window) {
        x3::logError("glfwCreateWindow failed");
        glfwTerminate();
        return 1;
    }

    // ---- Render device ----
    std::unique_ptr<x3::rhi::IRenderDevice> device(x3::rhi::createRenderDevice());

    x3::rhi::DeviceDesc desc{};
    desc.nativeWindowHandle = glfwGetWin32Window(window);
    desc.width  = W;
    desc.height = H;
    desc.vsync  = true;
#ifdef _DEBUG
    desc.validation = true;
#else
    desc.validation = false;
#endif

    if (!device->init(desc)) {
        x3::logError("render device init failed");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ---- Asset source (stub until D5) ----
    std::unique_ptr<x3::asset::IAssetSource> assets(x3::asset::createAssetSource());
    assets->mountPak("base.x3pak", 0);  // stub: logs not-implemented for now

    // ---- Physics world (M3 / Jolt) ----
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    if (!physics->init()) {
        x3::logError("physics world init failed");
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ---- Build the S2 graybox test level into the scene ----
    x3::game::Scene scene;
    x3::game::buildTestLevel(scene, *device, *physics);

    // ---- S4: a sliding door filling the doorway gap + a wall button linked to
    // it. Press E while aiming at the button to slide the door open. ----
    x3::game::DoorSystem doors;
    x3::game::buildDoorAndButton(scene, doors, *device, *physics);

    // ---- One dynamic box dropped above the floor: proves render+physics+entity
    // sync work together (it falls and rests on the floor). ----
    x3::rhi::TextureHandle boxTex;  // invalid => flat color via baseColor
    {
        x3::prims::PrimMesh boxGeo = x3::prims::makeBox(0.4f, 0.4f, 0.4f, 0,0,0, 1.0f);
        x3::game::Entity box;
        box.mesh = device->createMesh(boxGeo.verts.data(), (uint32_t)boxGeo.verts.size(),
                                      boxGeo.index.data(), (uint32_t)boxGeo.index.size());
        box.tex = boxTex;
        box.baseColor[0]=0.95f; box.baseColor[1]=0.35f; box.baseColor[2]=0.15f; box.baseColor[3]=1;
        // Dynamic body: 0.4 m half-extent box, mass 5 kg, dropped at y=4.
        const x3::phys::Vec3 dropPos{ 0.0f, 4.0f, 0.0f };
        box.body = physics->addBox(x3::phys::Vec3{0.4f,0.4f,0.4f}, dropPos, 5.0f,
                                   x3::phys::Layer::Dynamic);
        box.tag  = (uint32_t)x3::game::Tag::Prop;
        // Authored transform translation (overwritten each frame by Scene::update).
        box.transform[12] = dropPos.x;
        box.transform[13] = dropPos.y;
        box.transform[14] = dropPos.z;
        scene.add(box);
        x3::logInfo("dynamic box added at y=4.0 (will fall and rest on floor)");
    }

    // ---- S5: weapon pickup. Load the purchased WeaponEnergyPistol.glb (fallback
    // to a procedural box if it can't be loaded) and place a bobbing/spinning
    // pickup in open floor between the spawn and the doorway. Walk into it (within
    // ~1.2 m) to arm; the first-person viewmodel then appears. ----
    x3::game::WeaponSystem weapon;
    weapon.buildWeaponPickup(scene, *device, "G:/GameModels/rigged_glb",
                             x3::phys::Vec3{ 0.0f, 1.0f, 2.0f });

    if (smoketest) {
        x3::logInfo("smoketest: stepping physics + rendering 30 frames (+ a mid-run swapchain recreate)");
        // Arm the player so the weapon GLB renders as the viewmodel this run
        // (validates the real model loads + draws under validation). Use a fixed
        // camera looking toward the room so the viewmodel + pickup are on screen.
        weapon.forceArm(scene);
        const float vmX = -3.0f, vmY = 1.7f, vmZ = 4.0f, vmYaw = -1.4f, vmPitch = 0.0f;
        device->setCamera(vmX, vmY, vmZ, vmYaw, vmPitch, 60.0f);
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 30; ++i) {
            glfwPollEvents();
            if (i == 15) { x3::logInfo("smoketest: triggering swapchain recreate"); device->onResize(960, 540); }
            physics->step(dt);
            scene.update(*physics);
            weapon.update(dt, scene, x3::phys::Vec3{ vmX, vmY, vmZ });
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                weapon.drawPickup(*device, frame, scene);
                weapon.drawViewmodel(*device, frame, vmX, vmY, vmZ, vmYaw, vmPitch);
            }
            device->endFrame(frame);
        }
        x3::logInfo(std::string("smoketest: weapon viewmodel drawn (") +
                    (weapon.usingRealModel() ? "real GLB" : "fallback box") + ")");
        // Report where the dynamic box settled (proves physics drives the entity).
        for (uint32_t id = 0; id < scene.size(); ++id) {
            const auto& e = scene.get(id);
            if (e.tag == (uint32_t)x3::game::Tag::Prop && e.body.valid()) {
                x3::phys::Vec3 p = physics->getBodyPosition(e.body);
                x3::logInfo("smoketest: dynamic box settled at y=" + std::to_string(p.y));
            }
        }
        x3::logInfo("smoketest: 30 frames + recreate OK");
        physics->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    x3::logInfo("entering main loop — WASD walk, mouse look, LeftShift sprint, Space jump, E use, F noclip, Esc to quit");

    // ---- Walking player (S3). Spawn on open floor near the +Z doorway side,
    // clear of the falling box (origin) and the step platform (around x=3,z=-3).
    x3::game::Player player;
    player.spawn(*physics, -3.0f, 0.05f, 4.0f);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();

    // Rising-edge tracking for Space (jump), F (noclip toggle), E (use).
    bool prevSpace = false, prevF = false, prevE = false;

    // ---- Optional debug noclip/fly camera (toggle with F). Not required by S3,
    // handy for inspecting the level. Off by default — gameplay is the walker.
    bool noclip = false;
    float flyX = -3.0f, flyY = 1.7f, flyZ = 4.0f, flyYaw = 0.0f, flyPitch = 0.0f;

    // ---- Main loop ----
    int lastW = static_cast<int>(W), lastH = static_cast<int>(H);
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, 1);

        double nowT = glfwGetTime();
        float dt = static_cast<float>(nowT - prevTime); prevTime = nowT;
        if (dt > 0.1f) dt = 0.1f; // clamp huge hitches (e.g. after a stall)

        // Mouse delta this frame.
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = static_cast<float>(mx - lastMX), ddy = static_cast<float>(my - lastMY);
        lastMX = mx; lastMY = my;

        // F: toggle noclip on the rising edge. Seed the fly camera from the
        // player's current view so the transition is seamless.
        bool fNow = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
        if (fNow && !prevF) {
            noclip = !noclip;
            if (noclip) player.camera(flyX, flyY, flyZ, flyYaw, flyPitch);
            x3::logInfo(noclip ? "noclip ON" : "noclip OFF");
        }
        prevF = fNow;

        bool spaceNow = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;

        // ---- E: "use" on the rising edge. Raycast from the eye along the facing
        // direction; if it hits a button (within ~3 m) the linked door opens. ----
        bool eNow = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;
        if (eNow && !prevE) {
            float ex, ey, ez, yaw, pitch;
            player.camera(ex, ey, ez, yaw, pitch);   // in noclip the camera is the fly cam
            if (noclip) { ex = flyX; ey = flyY; ez = flyZ; yaw = flyYaw; pitch = flyPitch; }
            // Device forward convention: fwd = (cos p cos y, sin p, cos p sin y).
            x3::phys::Vec3 eye{ ex, ey, ez };
            x3::phys::Vec3 dir{ std::cos(pitch) * std::cos(yaw),
                                std::sin(pitch),
                                std::cos(pitch) * std::sin(yaw) };
            if (x3::game::tryUse(eye, dir, 3.0f, scene, doors, *physics))
                x3::logInfo("use: button pressed — door opening");
        }
        prevE = eNow;

        // Camera state this frame (set by whichever branch runs), reused below
        // for the weapon viewmodel.
        float camX, camY, camZ, camYaw, camPitch;
        if (!noclip) {
            // ---- Walking player input ----
            x3::game::PlayerInput in;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) in.moveFwd    += 1.0f;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) in.moveFwd    -= 1.0f;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) in.moveStrafe += 1.0f;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) in.moveStrafe -= 1.0f;
            in.sprint      = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
            in.jumpPressed = spaceNow && !prevSpace;   // rising edge
            in.lookDX = ddx;
            in.lookDY = ddy;

            player.update(in, dt, *physics);
            doors.update(dt, scene, *physics);   // advance any opening door first
            physics->step(dt);
            scene.update(*physics);

            player.camera(camX, camY, camZ, camYaw, camPitch);
            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
        } else {
            // ---- Debug fly camera (does not move the player body) ----
            const float sens = 0.0025f;
            flyYaw += ddx * sens; flyPitch -= ddy * sens;
            if (flyPitch >  1.55f) flyPitch =  1.55f;
            if (flyPitch < -1.55f) flyPitch = -1.55f;
            float fx = std::cos(flyPitch) * std::cos(flyYaw);
            float fy = std::sin(flyPitch);
            float fz = std::cos(flyPitch) * std::sin(flyYaw);
            float rl = std::sqrt(fx * fx + fz * fz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -fz / rl, rz = fx / rl;
            float spd = 4.0f * dt;
            if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) spd *= 3.0f;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { flyX += fx*spd; flyY += fy*spd; flyZ += fz*spd; }
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { flyX -= fx*spd; flyY -= fy*spd; flyZ -= fz*spd; }
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { flyX += rx*spd; flyZ += rz*spd; }
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { flyX -= rx*spd; flyZ -= rz*spd; }
            if (spaceNow) flyY += spd;
            if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) flyY -= spd;

            // World still advances so props keep simulating while inspecting.
            doors.update(dt, scene, *physics);   // advance any opening door first
            physics->step(dt);
            scene.update(*physics);
            device->setCamera(flyX, flyY, flyZ, flyYaw, flyPitch, 60.0f);
            camX = flyX; camY = flyY; camZ = flyZ; camYaw = flyYaw; camPitch = flyPitch;
        }
        prevSpace = spaceNow;

        // ---- S5: weapon pickup detection + viewmodel. Arm when the camera
        // (player eye) is within pickup radius; once armed, draw the viewmodel.
        weapon.update(dt, scene, x3::phys::Vec3{ camX, camY, camZ });

        int cw, ch;
        glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) {
            lastW = cw; lastH = ch;
            if (cw > 0 && ch > 0) device->onResize(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch));
        }

        auto frame = device->beginFrame();
        if (frame.valid) {
            scene.render(*device, frame);
            weapon.drawPickup(*device, frame, scene);   // bobbing pickup (until armed)
            weapon.drawViewmodel(*device, frame, camX, camY, camZ, camYaw, camPitch);
        }
        device->endFrame(frame);
    }

    x3::logInfo("shutting down");
    physics->shutdown();
    device->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
