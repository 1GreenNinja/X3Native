// X3Engine host — opens a window, brings up the render device, runs the loop.
// SKELETON: proves window + Vulkan device creation. Rendering is TODO (D1).

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

#include <memory>
#include <string_view>
#include <string>
#include <cmath>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace {

// ---- Procedural scene helpers (S1) ----------------------------------------
using x3::rhi::MeshVertex;

// A flat ground quad on the XZ plane, centered at origin, `half` units to a side,
// UVs tiled `tiles` times so the checker reads as repeated cells.
void makeGroundQuad(float half, float tiles,
                    std::vector<MeshVertex>& verts, std::vector<uint32_t>& idx) {
    verts = {
        {{-half, 0, -half}, {0, 1, 0}, {0,     0    }},
        {{ half, 0, -half}, {0, 1, 0}, {tiles, 0    }},
        {{ half, 0,  half}, {0, 1, 0}, {tiles, tiles}},
        {{-half, 0,  half}, {0, 1, 0}, {0,     tiles}},
    };
    // CCW when viewed from above (+Y), matching VK_FRONT_FACE_COUNTER_CLOCKWISE.
    idx = { 0, 2, 1, 0, 3, 2 };
}

// A unit cube (24 verts, per-face normals + UVs), `h` = half-extent.
void makeCube(float h, std::vector<MeshVertex>& verts, std::vector<uint32_t>& idx) {
    verts.clear(); idx.clear();
    auto face = [&](float ax,float ay,float az, float bx,float by,float bz,
                    float cx,float cy,float cz, float dx,float dy,float dz,
                    float nx,float ny,float nz) {
        uint32_t base = (uint32_t)verts.size();
        verts.push_back({{ax,ay,az},{nx,ny,nz},{0,0}});
        verts.push_back({{bx,by,bz},{nx,ny,nz},{1,0}});
        verts.push_back({{cx,cy,cz},{nx,ny,nz},{1,1}});
        verts.push_back({{dx,dy,dz},{nx,ny,nz},{0,1}});
        idx.insert(idx.end(), {base, base+1, base+2, base, base+2, base+3});
    };
    face(-h,-h, h,  h,-h, h,  h, h, h, -h, h, h,  0,0, 1); // +Z
    face( h,-h,-h, -h,-h,-h, -h, h,-h,  h, h,-h,  0,0,-1); // -Z
    face( h,-h, h,  h,-h,-h,  h, h,-h,  h, h, h,  1,0, 0); // +X
    face(-h,-h,-h, -h,-h, h, -h, h, h, -h, h,-h, -1,0, 0); // -X
    face(-h, h, h,  h, h, h,  h, h,-h, -h, h,-h,  0,1, 0); // +Y
    face(-h,-h,-h,  h,-h,-h,  h,-h, h, -h,-h, h,  0,-1,0); // -Y
}

// Procedural NxN checker texture (RGBA8). Two contrasting colors per cell.
std::vector<uint8_t> makeCheckerRGBA(uint32_t n, uint32_t cell) {
    std::vector<uint8_t> px((size_t)n * n * 4);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            bool on = ((x / cell) ^ (y / cell)) & 1u;
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            if (on) { p[0]=230; p[1]=230; p[2]=235; }     // light
            else    { p[0]= 40; p[1]= 55; p[2]= 90;  }    // dark blue-grey
            p[3] = 255;
        }
    return px;
}

// Pick the first .glb in the corpus directory (alphabetically), if any.
std::string firstGlbIn(const char* dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return {};
    std::string best;
    for (auto& de : fs::directory_iterator(dir, ec)) {
        if (de.path().extension() == ".glb") {
            std::string fn = de.path().filename().string();
            if (best.empty() || fn < best) best = fn;
        }
    }
    return best;
}

} // namespace

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

    // ---- Build the S1 scene: ground quad (checker), flat-color cube, one GLB ----
    const char* kCorpusDir = "G:/GameModels/rigged_glb";

    // (a) Ground quad + procedural checker texture.
    std::vector<MeshVertex> gv; std::vector<uint32_t> gi;
    makeGroundQuad(20.0f, 16.0f, gv, gi);
    x3::rhi::MeshHandle groundMesh = device->createMesh(gv.data(), (uint32_t)gv.size(),
                                                        gi.data(), (uint32_t)gi.size());
    std::vector<uint8_t> checker = makeCheckerRGBA(256, 32);
    x3::rhi::TextureHandle checkerTex = device->createTexture(checker.data(), 256, 256, true);

    // (b) Flat-color cube (default white texture x baseColorFactor).
    std::vector<MeshVertex> cv; std::vector<uint32_t> ci;
    makeCube(0.5f, cv, ci);
    x3::rhi::MeshHandle cubeMesh = device->createMesh(cv.data(), (uint32_t)cv.size(),
                                                      ci.data(), (uint32_t)ci.size());

    // (c) One GLB from the corpus, loaded STATIC (no skinning) via the M2 loader.
    std::unique_ptr<x3::asset::IAssetSource> modelAssets;
    std::unique_ptr<x3::asset::IModelLoader> loader;
    x3::asset::Model glbModel;
    std::vector<x3::asset::ModelDrawable> glbDrawables;
    std::string glbName = firstGlbIn(kCorpusDir);
    if (!glbName.empty()) {
        modelAssets.reset(x3::asset::createAssetSource());
        modelAssets->mountDir(kCorpusDir, 0);
        loader.reset(x3::asset::createModelLoader(device.get(), modelAssets.get()));
        glbModel = loader->load(glbName);
        if (glbModel.ok) {
            glbDrawables = x3::asset::makeDrawables(glbModel);
            x3::logInfo("S1 scene: loaded GLB '" + glbName + "' (" +
                        std::to_string(glbDrawables.size()) + " drawable primitives)");
        } else {
            x3::logWarn("S1 scene: GLB load failed: " + glbName);
        }
    } else {
        x3::logWarn(std::string("S1 scene: no .glb found in ") + kCorpusDir + " — drawing quad + cube only");
    }

    // Column-major transforms (translate only) for the three placements.
    auto translate = [](float x, float y, float z, float s, float out[16]) {
        out[0]=s; out[1]=0; out[2]=0; out[3]=0;
        out[4]=0; out[5]=s; out[6]=0; out[7]=0;
        out[8]=0; out[9]=0; out[10]=s; out[11]=0;
        out[12]=x; out[13]=y; out[14]=z; out[15]=1;
    };
    float groundXf[16], cubeXf[16], glbXf[16];
    translate(0.0f, 0.0f,  0.0f, 1.0f, groundXf);
    translate(-1.5f, 0.5f, 0.0f, 1.0f, cubeXf);
    translate( 1.8f, 0.0f, 0.0f, 1.0f, glbXf);

    const float kGroundFactor[4] = { 1, 1, 1, 1 };       // checker as-is
    const float kCubeFactor[4]   = { 0.95f, 0.55f, 0.2f, 1 }; // warm flat color

    // Issue all per-frame draws between beginFrame/endFrame.
    auto drawScene = [&](const x3::rhi::FrameContext& frame) {
        if (groundMesh.valid())
            device->drawMesh(frame, groundMesh, checkerTex, kGroundFactor, groundXf);
        if (cubeMesh.valid())
            device->drawMesh(frame, cubeMesh, x3::rhi::TextureHandle{}, kCubeFactor, cubeXf);
        for (const auto& d : glbDrawables)
            device->drawMesh(frame, x3::rhi::MeshHandle{ d.meshId },
                             x3::rhi::TextureHandle{ d.baseColorTexId },
                             d.baseColorFactor, glbXf);
    };

    // Destroys the procedurally-created scene resources (the GLB is freed via
    // loader->unload, which routes through the device's destroyMesh/Texture).
    auto destroyScene = [&]() {
        if (loader && glbModel.ok) loader->unload(glbModel);
        if (cubeMesh.valid())   device->destroyMesh(cubeMesh);
        if (groundMesh.valid()) device->destroyMesh(groundMesh);
        if (checkerTex.valid()) device->destroyTexture(checkerTex);
    };

    if (smoketest) {
        x3::logInfo("smoketest: rendering 30 frames (+ a mid-run swapchain recreate) then exiting");
        for (int i = 0; i < 30; ++i) {
            glfwPollEvents();
            if (i == 15) { x3::logInfo("smoketest: triggering swapchain recreate"); device->onResize(960, 540); }
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            device->endFrame(frame);
        }
        x3::logInfo("smoketest: 30 frames + recreate OK");
        destroyScene();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    x3::logInfo("entering main loop — WASD move, mouse look, Space/Ctrl up-down, Esc to quit");

    // ---- FPS free-look camera ----
    float camX = 0.0f, camY = 1.5f, camZ = 4.0f;
    float yaw = -1.5708f, pitch = -0.30f;   // matches the device's forward convention
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

        auto frame = device->beginFrame();
        if (frame.valid) drawScene(frame);
        device->endFrame(frame);
    }

    x3::logInfo("shutting down");
    destroyScene();
    device->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
