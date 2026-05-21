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

#include <memory>
#include <string_view>
#include <string>
#include <cmath>
#include <vector>
#include <cstdint>

int main(int argc, char** argv) {
    bool smoketest = false, testAsset = false, testConsole = false, testPhysics = false, testGltf = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == "--smoketest") smoketest = true;
        else if (a == "--test-asset") testAsset = true;
        else if (a == "--test-console") testConsole = true;
        else if (a == "--test-physics") testPhysics = true;
        else if (a == "--test-gltf") testGltf = true;
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

    if (smoketest) {
        x3::logInfo("smoketest: stepping physics + rendering 30 frames (+ a mid-run swapchain recreate)");
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 30; ++i) {
            glfwPollEvents();
            if (i == 15) { x3::logInfo("smoketest: triggering swapchain recreate"); device->onResize(960, 540); }
            physics->step(dt);
            scene.update(*physics);
            auto frame = device->beginFrame();
            if (frame.valid) scene.render(*device, frame);
            device->endFrame(frame);
        }
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

    x3::logInfo("entering main loop — WASD move, mouse look, Space/Ctrl up-down, Esc to quit");

    // ---- FPS free-look (fly) camera. Walking is S3. ----
    float camX = 0.0f, camY = 1.7f, camZ = 6.0f;
    float yaw = -1.5708f, pitch = -0.15f;   // matches the device's forward convention
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();

    // ---- Main loop ----
    int lastW = static_cast<int>(W), lastH = static_cast<int>(H);
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, 1);

        double nowT = glfwGetTime();
        float dt = static_cast<float>(nowT - prevTime); prevTime = nowT;

        // Mouse look
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = static_cast<float>(mx - lastMX), ddy = static_cast<float>(my - lastMY);
        lastMX = mx; lastMY = my;
        const float sens = 0.0025f;
        yaw += ddx * sens; pitch -= ddy * sens;
        if (pitch >  1.55f) pitch =  1.55f;
        if (pitch < -1.55f) pitch = -1.55f;

        // Forward + right (same convention the device uses)
        float fx = std::cos(pitch) * std::cos(yaw);
        float fy = std::sin(pitch);
        float fz = std::cos(pitch) * std::sin(yaw);
        float rl = std::sqrt(fx * fx + fz * fz); if (rl < 1e-4f) rl = 1e-4f;
        float rx = -fz / rl, rz = fx / rl;

        // Movement
        float spd = 4.0f * dt;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) spd *= 3.0f;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) { camX += fx*spd; camY += fy*spd; camZ += fz*spd; }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) { camX -= fx*spd; camY -= fy*spd; camZ -= fz*spd; }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) { camX += rx*spd; camZ += rz*spd; }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) { camX -= rx*spd; camZ -= rz*spd; }
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) camY += spd;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) camY -= spd;

        device->setCamera(camX, camY, camZ, yaw, pitch, 60.0f);

        int cw, ch;
        glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) {
            lastW = cw; lastH = ch;
            if (cw > 0 && ch > 0) device->onResize(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch));
        }

        // Advance physics, sync entity transforms, then draw the scene.
        physics->step(dt);
        scene.update(*physics);

        auto frame = device->beginFrame();
        if (frame.valid) scene.render(*device, frame);
        device->endFrame(frame);
    }

    x3::logInfo("shutting down");
    physics->shutdown();
    device->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
