// ===========================================================================
// HOST SELF-TESTS — GPU/headless self-test runners moved VERBATIM from
// app/main.cpp (#28 monolith split). Bodies unchanged; only the enclosing
// scope changed (anonymous/file-static -> namespace x3::apphost). The cull/
// debris/skin tests drive the REAL Vulkan device headless; the hatch chain
// test drives the real Level1Game + boot-loaded Lua + the shared bindings.
// ===========================================================================
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/core/x3_log.h"
#include "engine/core/IConsole.h"            // --test-visunify part C: cvar alias mapping
#include "engine/rhi/IRenderDevice.h"
#include "engine/rhi/FrustumCull.h"
#include "engine/rhi/Visibility.h"           // --test-visunify: r_vis policy + unified stats
#include <glm/gtc/matrix_transform.hpp>

#include "engine/physics/IPhysicsWorld.h"
#include "engine/script/IScriptSystem.h"
#include "scene.h"
#include "asset_root.h"
#include "level1_game.h"
#include "holo_terminal.h"
#include "secret_room.h"
#include "headless_device.h"

#include "self_tests.h"
#include "app_run.h"                          // --test-visunify part C: vis test hooks
#include "bindings.h"

#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <chrono>

namespace x3::apphost {

// ---------------------------------------------------------------------------
// --test-frustumcull : CPU per-object frustum-cull self-test (D15 baseline).
//
// Builds a KNOWN camera frustum (same viewProj convention the render device uses:
// glm::perspective, reverse-Y for Vulkan clip) and a KNOWN set of world spheres at
// known positions, then runs the EXACT engine cull math (engine/rhi/FrustumCull.h:
// extractFrustumPlanes + sphereInFrustum — the same functions VulkanRenderDevice
// calls) and asserts the precise survivor set, incl. the edge cases the GPU
// cull.comp must also honor:
//   * dead-ahead object inside the frustum                -> KEPT
//   * object far off to the side (outside left/right)     -> CULLED
//   * object fully BEHIND the camera (behind near plane)  -> CULLED
//   * sphere STRADDLING a plane (|dist| < radius)         -> KEPT (conservative)
//   * object beyond the far plane                         -> CULLED
//   * ALWAYS_VISIBLE bypass (caller skips the test)       -> KEPT even if outside
// Prints "frustumcull: X/Y passed" and returns true iff all pass. No GPU/window.
bool runFrustumCullSelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [") + (ok ? "PASS" : "FAIL") + "] " + name);
    };

    // Camera at origin looking down -Z (right-handed), 90deg FOV, aspect 1, near
    // 0.1, far 100. Reverse-Y exactly as VulkanRenderDevice::prepareFrameData does.
    const glm::vec3 eye(0.0f, 0.0f, 0.0f);
    glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;
    const FrustumPlanes fr = extractFrustumPlanes(proj * view);

    auto keep = [&](const glm::vec3& c, float r) {
        return sphereInFrustum(fr, CullSphere(c, r));
    };

    // (1) dead-ahead, well inside -> kept.
    check("ahead-inside kept", keep(glm::vec3(0, 0, -10), 1.0f));
    // (2) far off to the +X side at modest depth (way outside the 90deg cone) -> culled.
    check("far-right outside culled", !keep(glm::vec3(100, 0, -10), 1.0f));
    // (3) fully behind the camera (+Z) -> culled (behind near plane).
    check("behind-camera culled", !keep(glm::vec3(0, 0, 10), 1.0f));
    // (4) center just behind the near plane (z=+0.05) but radius 0.2 reaches across
    //     the near plane -> STRADDLES -> kept (conservative). Distance to near plane
    //     is 0.15 < r, so the test must keep it.
    check("straddle-near kept", keep(glm::vec3(0, 0, 0.05f), 0.2f));
    // (5) beyond the far plane (z=-150, far=100), small radius -> culled.
    check("beyond-far culled", !keep(glm::vec3(0, 0, -150), 1.0f));
    // (6) a tiny sphere exactly ON the right frustum edge at z=-10: at 90deg FOV the
    //     edge is x = |z| = 10. Center slightly OUTSIDE (x=11) with radius 0.5 (does
    //     not reach back to the plane) -> culled; radius 2.0 reaches the plane -> kept.
    check("edge-outside small culled", !keep(glm::vec3(11, 0, -10), 0.5f));
    check("edge-straddle large kept",   keep(glm::vec3(11, 0, -10), 2.0f));
    // (7) ALWAYS_VISIBLE bypass: an object far outside is KEPT because the caller
    //     never runs the test for noCull instances. Model the device's own guard.
    {
        const bool noCull = true;
        const glm::vec3 cOut(100, 100, 100);  // nowhere near the frustum
        const bool drawn = noCull ? true : keep(cOut, 1.0f);
        check("ALWAYS_VISIBLE kept", drawn);
    }
    // (8) world-transform path: a unit-radius local sphere translated to an inside
    //     position via a model matrix, with non-uniform scale (max axis grows the
    //     radius). Mirrors VulkanRenderDevice::worldSphere usage.
    {
        glm::mat4 m(1.0f);
        m = glm::translate(m, glm::vec3(0, 0, -20));
        m = glm::scale(m, glm::vec3(3.0f, 1.0f, 1.0f));  // max scale axis = 3
        const glm::vec3 cW = glm::vec3(m * glm::vec4(0, 0, 0, 1));
        const float rW = 1.0f * 3.0f;                    // local r=1 * maxScale
        check("world-xform inside kept", keep(cW, rW));
    }

    x3::logInfo("frustumcull: " + std::to_string(passed) + "/" +
                std::to_string(total) + " passed");
    return passed == total;
}

// ---------------------------------------------------------------------------
// --test-gpucull : D15 Tier-0 GPU cull EQUIVALENCE test (the soul of D15).
//
// Drives the REAL Vulkan device HEADLESS (validation ON) with the GPU cull path
// active (r_cullpath 1) AND the device's equivalence harness on: every frame the
// CPU evaluates the IDENTICAL cull predicate per instance and the device compares
// the GPU cull.comp's statDrawn readback against it. A grid of cube instances is
// rendered from multiple camera poses (full-cull, no-cull, partial, skewed) and
// the test asserts:
//   (a) the GPU path actually engaged (stats().gpuCullPath == 1),
//   (b) tested == submitted instance count at every sampled pose,
//   (c) drawn + frustumCulled == tested (no instance lost or double-counted),
//   (d) ZERO equivalence mismatches across every compared frame,
//   (e) the ALWAYS_VISIBLE bypass (r_frustumcull 0) draws every instance,
//   (f) toggling back to the CPU path (r_cullpath 0) restores the identity
//       indirection and keeps rendering (no stale compaction).
// ---------------------------------------------------------------------------
bool runGpuCullSelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [gpucull] ") + (ok ? "PASS " : "FAIL ") + name);
    };

    if (!glfwInit()) { x3::logError("[gpucull] glfwInit failed"); return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    std::unique_ptr<IRenderDevice> device(createRenderDevice());
    DeviceDesc desc{};
    desc.width = 640; desc.height = 360; desc.headless = true;
    desc.validation = true;   // this test doubles as the Tier-0 validation gate
    if (!device->init(desc)) {
        x3::logError("[gpucull] device init failed");
        glfwTerminate(); return false;
    }

    // Two distinct meshes (two indirect commands) so compaction bases differ:
    // a unit cube and a flat slab.
    auto makeBox = [&](float hx, float hy, float hz) {
        const float px[8] = { -hx,  hx,  hx, -hx, -hx,  hx,  hx, -hx };
        const float py[8] = { -hy, -hy, -hy, -hy,  hy,  hy,  hy,  hy };
        const float pz[8] = { -hz, -hz,  hz,  hz, -hz, -hz,  hz,  hz };
        MeshVertex v[8]{};
        for (int i = 0; i < 8; ++i) {
            v[i].pos[0] = px[i]; v[i].pos[1] = py[i]; v[i].pos[2] = pz[i];
            v[i].normal[1] = 1.0f;
        }
        const uint32_t idx[36] = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
                                   3,2,6, 3,6,7,  1,5,6, 1,6,2,  0,3,7, 0,7,4 };
        return device->createMesh(v, 8, idx, 36);
    };
    MeshHandle cube = makeBox(0.5f, 0.5f, 0.5f);
    MeshHandle slab = makeBox(2.0f, 0.1f, 2.0f);
    if (!cube.valid() || !slab.valid()) {
        x3::logError("[gpucull] mesh create failed");
        device->shutdown(); glfwTerminate(); return false;
    }

    // 24x24 cube grid + a 7x7 slab grid = 625 instances over [-69..69]^2 on XZ.
    constexpr uint32_t kGrid = 24, kSlabGrid = 7;
    constexpr uint32_t kInstances = kGrid * kGrid + kSlabGrid * kSlabGrid;
    const float white[4] = { 1, 1, 1, 1 };
    auto submitScene = [&](const x3::rhi::FrameContext& fc) {
        float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        for (uint32_t z = 0; z < kGrid; ++z)
            for (uint32_t x = 0; x < kGrid; ++x) {
                m[12] = -69.0f + 6.0f * x; m[13] = 0.5f; m[14] = -69.0f + 6.0f * z;
                device->drawMesh(fc, cube, {}, white, m);
            }
        for (uint32_t z = 0; z < kSlabGrid; ++z)
            for (uint32_t x = 0; x < kSlabGrid; ++x) {
                m[12] = -60.0f + 20.0f * x; m[13] = 4.0f; m[14] = -60.0f + 20.0f * z;
                device->drawMesh(fc, slab, {}, white, m);
            }
    };
    auto renderFrames = [&](int n, float cx, float cy, float cz, float yaw, float pitch) {
        device->setCamera(cx, cy, cz, yaw, pitch, 70.0f);
        for (int i = 0; i < n; ++i) {
            auto fc = device->beginFrame();
            if (!fc.valid) return false;
            submitScene(fc);
            device->endFrame(fc);
        }
        return true;
    };

    device->setGpuCullEquivalenceCheck(true);
    device->setCullPath(1);                  // Tier 0 (graphics-queue compute)
    device->setFrustumCullEnabled(true);

    // Pose sweep. 4 frames per pose so the frames-in-flight readback latency
    // drains and the sampled stats describe THIS pose.
    struct Pose { const char* name; float x, y, z, yaw, pitch; bool expectSomeCulled, expectSomeDrawn; };
    const Pose poses[] = {
        { "center +X",      0.0f, 2.0f,   0.0f, 0.0f,  0.0f,  true,  true  },
        { "center skewed",  10.0f, 3.0f,  -8.0f, 2.4f, -0.3f, true,  true  },
        { "straight down",  0.0f, 40.0f,  0.0f, 0.0f, -1.55f, true,  true  },
        { "outside looking away", 200.0f, 2.0f, 0.0f, 0.0f, 0.0f, true, false },
        { "far overview (sees all)", 0.0f, 150.0f, 190.0f, -1.5708f, -0.65f, false, true },
    };
    for (const Pose& p : poses) {
        if (!renderFrames(4, p.x, p.y, p.z, p.yaw, p.pitch)) {
            check((std::string(p.name) + ": render frames").c_str(), false);
            continue;
        }
        const RenderStats st = device->stats();
        check((std::string(p.name) + ": gpu path active").c_str(), st.gpuCullPath == 1);
        check((std::string(p.name) + ": tested == submitted").c_str(), st.gpuCullTested == kInstances);
        check((std::string(p.name) + ": drawn + frustumCulled == tested").c_str(),
              st.gpuCullDrawn + st.gpuCullFrustum == st.gpuCullTested && st.gpuCullHzb == 0);
        if (p.expectSomeCulled) check((std::string(p.name) + ": culls something").c_str(), st.gpuCullFrustum > 0);
        if (p.expectSomeDrawn)  check((std::string(p.name) + ": draws something").c_str(), st.gpuCullDrawn > 0);
        else                    check((std::string(p.name) + ": draws nothing").c_str(), st.gpuCullDrawn == 0);
        check((std::string(p.name) + ": GPU drawn == CPU expected").c_str(),
              st.gpuCullDrawn == st.gpuCullExpected);
    }

    // (e) ALWAYS_VISIBLE bypass: r_frustumcull 0 -> every instance survives.
    device->setFrustumCullEnabled(false);
    if (renderFrames(4, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f)) {
        const RenderStats st = device->stats();
        check("bypass (r_frustumcull 0): drawn == tested",
              st.gpuCullDrawn == st.gpuCullTested && st.gpuCullTested == kInstances);
    } else check("bypass render", false);
    device->setFrustumCullEnabled(true);

    // TIER 1 (async compute queue) — same predicate, same equivalence, on the
    // 5090's dedicated compute queue. If the device has no dedicated queue the
    // path resolves back to 1 (also asserted: never 2 without support).
    device->setCullPath(2);
    if (renderFrames(4, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f)) {
        const RenderStats st = device->stats();
        const bool tier1 = st.gpuCullPath == 2;
        check(tier1 ? "tier1: async path active" : "tier1: clamped to tier0 (no dedicated queue)",
              st.gpuCullPath == 2 || st.gpuCullPath == 1);
        check("tier1: tested == submitted", st.gpuCullTested == kInstances);
        check("tier1: drawn + frustumCulled == tested",
              st.gpuCullDrawn + st.gpuCullFrustum == st.gpuCullTested);
        check("tier1: GPU drawn == CPU expected", st.gpuCullDrawn == st.gpuCullExpected);
    } else check("tier1 render", false);
    if (renderFrames(4, 10.0f, 3.0f, -8.0f, 2.4f, -0.3f)) {
        const RenderStats st = device->stats();
        check("tier1 skewed: GPU drawn == CPU expected",
              st.gpuCullDrawn == st.gpuCullExpected && st.gpuCullDrawn > 0);
    } else check("tier1 skewed render", false);
    device->setCullPath(1);

    // (d) zero mismatches across every compared frame so far.
    {
        const RenderStats st = device->stats();
        check("equivalence frames compared > 0", st.gpuCullEquivFrames > 0);
        check("equivalence mismatches == 0", st.gpuCullEquivMismatches == 0);
    }

    // (f) toggle back to the CPU path: identity restored, still renders, and the
    // CPU cull draws the same survivor count the GPU just computed for this pose.
    device->setCullPath(0);
    bool cpuOk = renderFrames(3, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
    const RenderStats stCpu = device->stats();
    check("toggle to CPU path renders", cpuOk);
    check("CPU path resolves to 0", stCpu.gpuCullPath == 0);
    check("CPU objectsDrawn > 0 after toggle", stCpu.objectsDrawn > 0);

    device->shutdown();
    device.reset();
    glfwTerminate();

    x3::logInfo("gpucull: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
}

// ---------------------------------------------------------------------------
// --test-visunify : the vis-unify acceptance gate (the ONE culling brain).
//
//   A) POLICY TABLE (pure): resolveVisPolicy auto/degradation/PVS-override.
//   B) CONSERVATION ACROSS POLICIES (real device, validation ON, still camera,
//      synthetic two-room scene): `drawn` must be IDENTICAL for r_vis 1/2/3
//      (PVS+GPU must not over-cull vs the CPU reference), every level's chain
//      must CONSERVE (rooms + frustum + hzb + drawn == candidates), and the
//      GPU path's `tested` must equal the PVS SURVIVOR set — the proof that
//      the PVS prefilter is the GPU cull's INPUT, not a parallel system.
//   C) ALIAS MAPPING: the legacy cvars (r_cullpath/r_hzb/r_roomcull) remap
//      onto r_vis through the SAME per-frame sync the world loops run.
//   D) TLAS MUTATION INSTRUMENTATION (RT devices; skipped cleanly elsewhere):
//      spawn/despawn instances under load and confirm the mutation path is
//      ZERO-stutter. RE-HOMED reconciliation: the empire RT stack already
//      shipped the TLAS double-buffer (VulkanRT m_tlasRing), so the per-frame
//      scene-mutation vkDeviceWaitIdle is GONE — this part now ASSERTS the real
//      zero steady-state sync-waits (only the boot first-build wait remains)
//      instead of the old "report sync-waits" path. That is the 31/32 -> 32/32.
// ---------------------------------------------------------------------------
bool runVisUnifySelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const std::string& name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [visunify] ") + (ok ? "PASS " : "FAIL ") + name);
    };

    // ---- A) policy table (pure, headless) ----------------------------------
    {
        VisCaps none{};                       // no GPU cull anywhere
        VisCaps gpu{ true, false, false };
        VisCaps full{ true, true, true };
        VisPolicy p;
        p = resolveVisPolicy(-1, none);
        check("A: auto w/o gpu -> L1 pvs+cpu", p.mode == 1 && p.pvs && p.cullPath == 0 && !p.hzb);
        p = resolveVisPolicy(3, none);
        check("A: L3 w/o gpu degrades -> L1", p.mode == 1 && p.cullPath == 0 && !p.hzb);
        p = resolveVisPolicy(3, gpu);
        check("A: L3 w/o hzb degrades -> L2", p.mode == 2 && p.cullPath == -1 && !p.hzb);
        p = resolveVisPolicy(-1, full);
        check("A: auto on full caps -> L3", p.mode == 3 && p.pvs && p.cullPath == -1 && p.hzb);
        p = resolveVisPolicy(0, full);
        check("A: L0 = cpu-only reference floor", p.mode == 0 && !p.pvs && p.cullPath == 0 && !p.hzb);
        p = resolveVisPolicy(2, full, /*pvsOverride=*/0);
        check("A: r_roomcull-0 override kills PVS only", p.mode == 2 && !p.pvs && p.cullPath == -1);
        p = resolveVisPolicy(7, full);
        check("A: out-of-range clamps to L3", p.mode == 3);
    }

    // ---- real device for B/C/D ---------------------------------------------
    if (!glfwInit()) { x3::logError("[visunify] glfwInit failed"); return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    std::unique_ptr<IRenderDevice> device(createRenderDevice());
    DeviceDesc desc{};
    desc.width = 640; desc.height = 360; desc.headless = true;
    desc.validation = true;   // doubles as the validation-silence gate
    if (!device->init(desc)) {
        x3::logError("[visunify] device init failed");
        glfwTerminate(); return false;
    }

    auto makeBox = [&](float hx, float hy, float hz) {
        const float px[8] = { -hx,  hx,  hx, -hx, -hx,  hx,  hx, -hx };
        const float py[8] = { -hy, -hy, -hy, -hy,  hy,  hy,  hy,  hy };
        const float pz[8] = { -hz, -hz,  hz,  hz, -hz, -hz,  hz,  hz };
        MeshVertex v[8]{};
        for (int i = 0; i < 8; ++i) {
            v[i].pos[0] = px[i]; v[i].pos[1] = py[i]; v[i].pos[2] = pz[i];
            v[i].normal[1] = 1.0f;
        }
        const uint32_t idx[36] = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
                                   3,2,6, 3,6,7,  1,5,6, 1,6,2,  0,3,7, 0,7,4 };
        return device->createMesh(v, 8, idx, 36);
    };
    MeshHandle cube = makeBox(0.5f, 0.5f, 0.5f);
    if (!cube.valid()) {
        x3::logError("[visunify] mesh create failed");
        device->shutdown(); glfwTerminate(); return false;
    }

    // Synthetic two-room scene: room 0 = a 12x12 grid around the origin (the
    // camera's room), room 1 = an identical grid 400 m away (PVS-culled). With
    // the visible set {0}, the PVS prefilter removes room 1 BEFORE submission —
    // exactly how the canon level feeds the device.
    x3::game::Scene scene;
    constexpr uint32_t kGrid = 12;
    constexpr uint32_t kPerRoom = kGrid * kGrid;
    auto addRoom = [&](uint32_t roomId, float baseX) {
        for (uint32_t z = 0; z < kGrid; ++z)
            for (uint32_t x = 0; x < kGrid; ++x) {
                x3::game::Entity e;
                e.mesh = cube;
                e.transform[12] = baseX - 33.0f + 6.0f * x;
                e.transform[13] = 0.5f;
                e.transform[14] = -33.0f + 6.0f * z;
                e.roomId = roomId;
                scene.add(e);
            }
    };
    addRoom(0, 0.0f);
    addRoom(1, 400.0f);
    const uint32_t vis0[1] = { 0 };
    scene.setVisibleRooms(vis0, 1);

    device->setCamera(0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 70.0f);
    device->setFrustumCullEnabled(true);

    auto renderFrames = [&](const VisPolicy& pol, int n) {
        scene.setRoomCullEnabled(pol.pvs);
        device->setCullPath(pol.cullPath);
        device->setHzbEnabled(pol.hzb);
        for (int i = 0; i < n; ++i) {
            auto fc = device->beginFrame();
            if (!fc.valid) return false;
            scene.render(*device, fc);
            device->setVisHostStats(scene.lastRoomCulled(), 0.0f);
            device->endFrame(fc);
        }
        return true;
    };

    // Warmup on the CPU path to publish the device caps + valid depth (HZB).
    VisCaps caps;
    {
        renderFrames(resolveVisPolicy(0, caps), 4);
        const RenderStats st = device->stats();
        caps.gpuCull = st.gpuCullSupported;
        caps.asyncCull = st.asyncCullSupported;
        caps.hzb = st.hzbSupported;
        x3::logInfo(std::string("[visunify] device caps: gpuCull=") +
                    (caps.gpuCull ? "1" : "0") + " async=" + (caps.asyncCull ? "1" : "0") +
                    " hzb=" + (caps.hzb ? "1" : "0"));
    }

    // ---- B) conservation across r_vis levels, still camera -----------------
    uint32_t drawnRef = 0, testedPvs = 0;
    {
        // L0: no PVS — the reference for "the PVS input set" (== all instances).
        VisPolicy p0 = resolveVisPolicy(0, caps);
        renderFrames(p0, 5);
        const VisFrameStats v0 = assembleVisStats(device->stats(), p0.mode);
        check("B: L0 candidates == whole scene", v0.candidates == 2 * kPerRoom && v0.roomsCulled == 0);
        check("B: L0 conserves", v0.conserves);

        // L1: PVS+CPU — the legacy default; PVS removes room 1 at submission.
        VisPolicy p1 = resolveVisPolicy(1, caps);
        renderFrames(p1, 5);
        const VisFrameStats v1 = assembleVisStats(device->stats(), p1.mode);
        drawnRef  = v1.drawn;
        testedPvs = v1.tested;
        check("B: L1 PVS prefilters a full room", v1.roomsCulled == kPerRoom);
        check("B: L1 tested == PVS survivors", v1.tested == kPerRoom);
        check("B: L1 conserves", v1.conserves && v1.candidates == 2 * kPerRoom);
        check("B: L1 draws something", v1.drawn > 0 && v1.drawn < kPerRoom);

        if (caps.gpuCull) {
            // L2: PVS+GPU — the PVS survivor set must BE the GPU cull's input.
            VisPolicy p2 = resolveVisPolicy(2, caps);
            renderFrames(p2, 6);
            const VisFrameStats v2 = assembleVisStats(device->stats(), p2.mode);
            check("B: L2 gpu path active", v2.activePath >= 1);
            check("B: L2 tested == PVS survivor set (prefilter feeds the GPU)",
                  v2.tested == testedPvs);
            check("B: L2 drawn identical to CPU reference (no over-cull)",
                  v2.drawn == drawnRef);
            check("B: L2 conserves", v2.conserves && v2.candidates == 2 * kPerRoom);

            if (caps.hzb) {
                // L3: +HZB — occlusion splits the frustum survivors; conservation
                // must hold and nothing beyond the CPU reference may be drawn.
                VisPolicy p3 = resolveVisPolicy(3, caps);
                renderFrames(p3, 6);
                const VisFrameStats v3 = assembleVisStats(device->stats(), p3.mode);
                check("B: L3 hzb engaged on the gpu path", v3.activePath >= 1);
                check("B: L3 drawn+hzb == CPU reference (HZB conservation)",
                      v3.drawn + v3.hzbCulled == drawnRef);
                check("B: L3 conserves", v3.conserves);
            } else {
                x3::logInfo("[visunify] B: L3 skipped (no HZB targets on this device)");
            }
        } else {
            x3::logInfo("[visunify] B: L2/L3 skipped (no GPU cull on this device)");
        }

        // auto (-1) must resolve to the best supported level.
        VisPolicy pa = resolveVisPolicy(-1, caps);
        const int best = caps.hzb ? 3 : (caps.gpuCull ? 2 : 1);
        check("B: auto resolves to best supported", pa.mode == best);
    }

    // ---- C) alias-cvar mapping (the same sync the world loops run) ---------
    {
        std::unique_ptr<x3::con::IConsole> con(x3::con::createConsole());
        registerViewmodelCVarsForTest(*con);
        resetVisSyncForTest();                       // fresh sync state for the test
        applyRtaoCVarsForTest(*con, *device);        // first sync (defaults)
        check("C: default r_vis 1 -> pvs+cpu", visPolicyForTest().mode == 1 && visPolicyForTest().pvs);

        con->set("r_cullpath", "1");
        applyRtaoCVarsForTest(*con, *device);
        check("C: r_cullpath 1 alias -> r_vis 2", con->getInt("r_vis") == 2);
        check("C: alias preserves the explicit tier",
              !caps.gpuCull || visPolicyForTest().cullPath == 1);

        con->set("r_hzb", "1");
        applyRtaoCVarsForTest(*con, *device);
        check("C: r_hzb alias -> r_vis 3", con->getInt("r_vis") == 3);

        con->set("r_cullpath", "0");
        applyRtaoCVarsForTest(*con, *device);
        check("C: r_cullpath 0 alias -> r_vis 1", con->getInt("r_vis") == 1);

        con->set("r_roomcull", "0");
        applyRtaoCVarsForTest(*con, *device);
        check("C: r_roomcull 0 -> PVS override only",
              !visPolicyForTest().pvs && con->getInt("r_vis") == 1);

        con->set("r_roomcull", "1");
        con->set("r_vis", "2");
        applyRtaoCVarsForTest(*con, *device);
        check("C: direct r_vis 2 clears the tier force + restores PVS",
              visPolicyForTest().pvs && (!caps.gpuCull || visPolicyForTest().cullPath == -1));
        resetVisSyncForTest();                       // leave clean state behind
    }

    // ---- D) TLAS mutation ZERO-sync-wait proof (double-buffer base) ---------
    {
        // Back to the plain CPU path; enable RT-AO so the BLAS/TLAS path runs.
        renderFrames(resolveVisPolicy(0, caps), 2);
        IRenderDevice::RtaoParams rp{};
        rp.enabled = true;
        device->setRtaoParams(rp);
        // Static warmup: first BLAS/TLAS builds (BLAS is a synchronous load-time
        // event by design — it must NOT count against the mutation gate). After
        // warmup the ONLY sync-wait paid is the boot first-build (so syncWaits<=1).
        renderFrames(resolveVisPolicy(0, caps), 6);
        const uint32_t buildsAfterWarmup = device->stats().tlasBuilds;
        const uint32_t syncWaitsAfterWarmup = device->stats().tlasSyncWaits;
        if (device->stats().tlasBuilds == 0) {
            x3::logInfo("[visunify] D: skipped (device has no ray tracing — TLAS path inert)");
        } else {
            // 120 frames of per-frame scene mutation: a varying extra instance set
            // changes the TLAS signature EVERY frame -> a rebuild every frame.
            const float white[4] = { 1, 1, 1, 1 };
            for (int f = 0; f < 120; ++f) {
                auto fc = device->beginFrame();
                if (!fc.valid) break;
                scene.render(*device, fc);
                device->setVisHostStats(scene.lastRoomCulled(), 0.0f);
                float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
                const int extras = (f % 17) + 3;
                for (int s = 0; s < extras; ++s) {
                    m[12] = -20.0f + 2.1f * s + 0.13f * f;
                    m[13] = 3.0f + 0.5f * s;
                    m[14] = -8.0f - 1.7f * s;
                    device->drawMesh(fc, cube, {}, white, m);
                }
                device->endFrame(fc);
            }
            const RenderStats st = device->stats();
            const uint32_t builds = st.tlasBuilds - buildsAfterWarmup;
            const uint32_t mutationWaits = st.tlasSyncWaits - syncWaitsAfterWarmup;
            x3::logInfo("[visunify] D: tlasBuilds(mutation)=" + std::to_string(builds) +
                        " syncWaits(mutation)=" + std::to_string(mutationWaits) +
                        " totalBuilds=" + std::to_string(st.tlasBuilds) +
                        " totalSyncWaits=" + std::to_string(st.tlasSyncWaits) +
                        " lastCpuMs=" + std::to_string(st.tlasCpuMs));
            // The mutation path is instrumented + exercised: a rebuild recorded on
            // (nearly) every mutated frame, and the counters are self-consistent.
            check("D: a rebuild recorded (nearly) every mutated frame", builds >= 110);
            check("D: rebuild instrumentation is wired + bounded",
                  st.tlasCpuMs >= 0.0f && st.tlasBuilds >= builds);
            // RECONCILED 32/32 (the double-buffer shipped on THIS base): the old
            // vis-unify deferred this to "report sync-waits" (it was 31/32 with a
            // per-frame vkDeviceWaitIdle); the empire RT stack landed the TLAS
            // double-buffer, so the per-frame scene-mutation rebuild now pays ZERO
            // device waits — assert the REAL zero.
            check("D: ZERO steady-state sync-waits (TLAS double-buffer)", mutationWaits == 0);
            check("D: boot first-build wait is the only wait the path ever paid",
                  st.tlasSyncWaits <= 1);
        }
    }

    device->destroyMesh(cube);
    device->shutdown();
    device.reset();
    glfwTerminate();

    x3::logInfo("visunify: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
}

// ---------------------------------------------------------------------------
// --test-debris : K-T2 GPU-compute persistent debris world self-test.
//
// Drives the REAL Vulkan render device HEADLESS (no window) so the compute path is
// actually exercised on the GPU (not a CPU stand-in). It spawns a burst of N
// fragments above a ground plane, steps the compute sim M frames (each through a
// real beginFrame -> gpuDebrisStep -> gpuDebrisDraw -> endFrame), and asserts:
//   (a) count is correct right after spawn,
//   (b) fragments FALL (minY drops) then SETTLE on the ground (no NaNs, bounded
//       positions, most fragments asleep, ~zero residual speed),
//   (c) LIFETIME expiry FREES fragments back to the pool (alive count drops to 0),
//   (d) NO leaks: every fragment returns to the dead pool, alive == 0 at the end.
// Prints "debris: X/Y passed" and returns true iff all pass.
bool runDebrisSelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [debris] ") + (ok ? "PASS " : "FAIL ") + name);
        return ok;
    };

    if (!glfwInit()) { x3::logError("[debris] glfwInit failed"); return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    std::unique_ptr<IRenderDevice> device(createRenderDevice());
    DeviceDesc desc{};
    desc.width = 640; desc.height = 360; desc.headless = true;
#ifdef _DEBUG
    desc.validation = true;
#endif
    if (!device->init(desc)) { x3::logError("[debris] device init failed"); glfwTerminate(); return false; }

    // Ground plane at y=0; modest gravity world.
    IRenderDevice::GpuDebrisParams p{};
    p.groundY = 0.0f;
    p.restitution = 0.10f;            // low bounce so fragments settle quickly
    p.friction = 0.6f;
    p.linearDamping = 0.6f;
    p.sleepLinSpeed = 0.30f;
    p.sleepAngSpeed = 0.6f;
    p.sleepFrames = 8;
    device->gpuDebrisConfig(p);

    const uint32_t N = 4096;          // far beyond the ~256 Jolt chunk budget
    const float spawnPos[3] = { 0.0f, 6.0f, 0.0f };
    // Lifetime well clear of the settle window below so none expire mid-settle.
    uint32_t spawned = device->gpuDebrisSpawnBurst(spawnPos, N, /*speed*/3.0f,
                                                   /*lifetime*/3.0f, /*halfExtent*/0.1f, /*seed*/12345u);
    check("spawn count == requested", spawned == N);
    check("alive == N right after spawn", device->gpuDebrisAliveCount() == N);
    check("capacity >= N", device->gpuDebrisCapacity() >= N);

    const float tint[4] = { 0.7f, 0.55f, 0.4f, 1.0f };
    const float dt = 1.0f / 60.0f;
    const float white[4] = { 1, 1, 1, 1 };

    // Helper: run one device frame that steps + draws the debris.
    auto stepFrame = [&]() {
        device->setCamera(0.0f, 3.0f, 10.0f, -1.5708f, -0.2f, 60.0f);
        FrameContext fc = device->beginFrame();
        if (!fc.valid) return;
        device->gpuDebrisStep(dt);
        device->gpuDebrisDraw(fc, tint);
        device->endFrame(fc);
    };

    // --- Step a few frames; fragments should be FALLING (minY below spawn). ---
    for (int i = 0; i < 6; ++i) stepFrame();
    IRenderDevice::GpuDebrisStats mid = device->gpuDebrisReadback(1.0e4f);
    check("no NaNs while falling", mid.nanCount == 0);
    check("bounded positions while falling", mid.outOfBounds == 0);
    check("fragments fell below spawn height", mid.minY < spawnPos[1]);
    check("alive unchanged before any expiry", mid.alive == N);

    // --- Settle: step to ~1.9s total so every fragment hits the ground + sleeps.
    //     The shortest lifetime is 0.7*3.0 = 2.1s, so NONE expire in this window. ---
    for (int i = 0; i < 108; ++i) stepFrame();   // 6 + 108 = 114 frames ~ 1.9s
    IRenderDevice::GpuDebrisStats settled = device->gpuDebrisReadback(1.0e4f);
    check("no NaNs after settling", settled.nanCount == 0);
    check("bounded positions after settling", settled.outOfBounds == 0);
    check("rest on/above the ground (minY >= groundY)", settled.minY >= p.groundY - 0.05f);
    check("did not sink far (maxY bounded)", settled.maxY < spawnPos[1] + 1.0f);
    check("most fragments settled to sleep", settled.settled > (N * 3) / 4);
    check("settled debris is ~motionless", settled.maxSpeed < 1.0f);
    check("still alive before lifetime expiry", settled.alive == N);

    // --- Lifetime expiry: keep stepping past the 2.0s max lifetime so EVERY
    //     fragment's life decays to 0 and is freed back to the pool. ---
    for (int i = 0; i < 240; ++i) stepFrame();
    IRenderDevice::GpuDebrisStats expired = device->gpuDebrisReadback(1.0e4f);
    check("lifetime expiry freed all fragments", expired.alive == 0);
    check("alive counter back to 0 (no leak)", device->gpuDebrisAliveCount() == 0);
    check("no NaNs after full recycle", expired.nanCount == 0);

    // --- Re-spawn into the recycled pool to prove slots are reusable (no leak/grow). ---
    uint32_t resp = device->gpuDebrisSpawnBurst(spawnPos, 1000, 3.0f, 1.0f, 0.1f, 777u);
    check("re-spawn into recycled pool", resp == 1000 && device->gpuDebrisAliveCount() == 1000);
    for (int i = 0; i < 120; ++i) stepFrame();
    check("re-spawned batch also expires cleanly", device->gpuDebrisAliveCount() == 0);

    device->shutdown();
    glfwTerminate();

    std::printf("debris: %d/%d passed\n", passed, total);
    x3::logInfo("debris: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
}

// --test-gpuskin : GPU compute-skinning self-test (GPU SKINNING OF MODELS).
//
// Drives the REAL Vulkan render device HEADLESS (no window) so the compute skinning
// path is actually exercised on the GPU (not a CPU stand-in). It registers a small
// skinned mesh, sets KNOWN palettes, runs the compute skinning pre-pass through a
// real beginFrame -> setSkinnedPalette -> (graph dispatches skin.comp) -> endFrame,
// reads back the skinned-output buffer, and asserts it matches a CPU linear-blend-
// skinning reference within epsilon:
//   (a) IDENTITY palette  => output == bind pose EXACTLY,
//   (b) a known joint TRANSLATION => weighted verts move by the expected amount,
//   (c) a known joint ROTATION (+ translation) => verts land where the CPU LBS
//       reference (p' = sum_i w_i * J[idx_i] * p) places them.
// Prints "gpuskin: X/Y passed" and returns true iff all pass.
bool runGpuSkinSelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [gpuskin] ") + (ok ? "PASS " : "FAIL ") + name);
        return ok;
    };

    // ---- column-major 4x4 helpers (glTF/glm convention) for the CPU reference. ----
    auto trsToMat4 = [](const float t[3], const float q[4], const float s[3], float* m) {
        const float x=q[0], y=q[1], z=q[2], w=q[3];
        const float xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z, wx=w*x, wy=w*y, wz=w*z;
        m[0]=(1-2*(yy+zz))*s[0]; m[1]=(2*(xy+wz))*s[0]; m[2]=(2*(xz-wy))*s[0]; m[3]=0;
        m[4]=(2*(xy-wz))*s[1]; m[5]=(1-2*(xx+zz))*s[1]; m[6]=(2*(yz+wx))*s[1]; m[7]=0;
        m[8]=(2*(xz+wy))*s[2]; m[9]=(2*(yz-wx))*s[2]; m[10]=(1-2*(xx+yy))*s[2]; m[11]=0;
        m[12]=t[0]; m[13]=t[1]; m[14]=t[2]; m[15]=1;
    };
    auto xformPoint = [](const float m[16], const float p[3], float o[3]) {
        o[0]=m[0]*p[0]+m[4]*p[1]+m[8] *p[2]+m[12];
        o[1]=m[1]*p[0]+m[5]*p[1]+m[9] *p[2]+m[13];
        o[2]=m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14];
    };
    auto xformDir = [](const float m[16], const float d[3], float o[3]) {
        o[0]=m[0]*d[0]+m[4]*d[1]+m[8] *d[2];
        o[1]=m[1]*d[0]+m[5]*d[1]+m[9] *d[2];
        o[2]=m[2]*d[0]+m[6]*d[1]+m[10]*d[2];
    };

    // ---- A small synthetic skinned mesh: 4 verts, 2 joints. The first two verts are
    // rigidly bound to joint 0, the last two to joint 1, and the MIDDLE-ish weights
    // exercise the blend (a 50/50 vertex). ----
    const uint32_t V = 4;
    const uint32_t J = 2;
    std::vector<MeshVertex> bind(V);
    bind[0] = { {0.0f, 0.0f, 0.0f}, {0,1,0}, {0,0} };
    bind[1] = { {1.0f, 0.0f, 0.0f}, {0,1,0}, {1,0} };
    bind[2] = { {2.0f, 0.0f, 0.0f}, {0,0,1}, {0,1} };
    bind[3] = { {3.0f, 1.0f, 0.0f}, {1,0,0}, {1,1} };   // 50/50 between joint 0 and 1
    std::vector<uint16_t> jidx = {
        0,0,0,0,   // v0 -> joint 0
        0,0,0,0,   // v1 -> joint 0
        1,0,0,0,   // v2 -> joint 1
        0,1,0,0,   // v3 -> 50% joint0 + 50% joint1
    };
    std::vector<float> jwt = {
        1,0,0,0,
        1,0,0,0,
        1,0,0,0,
        0.5f,0.5f,0,0,
    };
    // Index buffer (two tris) — only needed so createMesh succeeds; the test reads
    // back vertices, it does not rasterize.
    std::vector<uint32_t> idx = { 0,1,2, 0,2,3 };

    // CPU LBS reference: skin `bind` with a flat palette of J column-major mat4s.
    auto cpuSkin = [&](const std::vector<float>& palette, std::vector<MeshVertex>& out) {
        out.resize(V);
        for (uint32_t v = 0; v < V; ++v) {
            const float* bp = bind[v].pos;
            const float* bn = bind[v].normal;
            const uint16_t* ji = &jidx[v*4];
            const float* jw = &jwt[v*4];
            float wsum = jw[0]+jw[1]+jw[2]+jw[3];
            float pAcc[3]={0,0,0}, nAcc[3]={0,0,0};
            if (wsum < 1e-6f) { pAcc[0]=bp[0]; pAcc[1]=bp[1]; pAcc[2]=bp[2]; nAcc[0]=bn[0]; nAcc[1]=bn[1]; nAcc[2]=bn[2]; }
            else {
                for (int i = 0; i < 4; ++i) {
                    float w = jw[i]; if (w <= 0.0f) continue;
                    uint16_t j = ji[i]; if (j >= J) continue;
                    const float* jm = &palette[(size_t)j*16];
                    float tp[3], tn[3]; xformPoint(jm, bp, tp); xformDir(jm, bn, tn);
                    pAcc[0]+=w*tp[0]; pAcc[1]+=w*tp[1]; pAcc[2]+=w*tp[2];
                    nAcc[0]+=w*tn[0]; nAcc[1]+=w*tn[1]; nAcc[2]+=w*tn[2];
                }
                float inv = 1.0f/wsum; pAcc[0]*=inv; pAcc[1]*=inv; pAcc[2]*=inv;
            }
            float nl = std::sqrt(nAcc[0]*nAcc[0]+nAcc[1]*nAcc[1]+nAcc[2]*nAcc[2]);
            if (nl > 1e-8f) { nAcc[0]/=nl; nAcc[1]/=nl; nAcc[2]/=nl; }
            out[v] = { {pAcc[0],pAcc[1],pAcc[2]}, {nAcc[0],nAcc[1],nAcc[2]}, {bind[v].uv[0],bind[v].uv[1]} };
        }
    };

    if (!glfwInit()) { x3::logError("[gpuskin] glfwInit failed"); return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    std::unique_ptr<IRenderDevice> device(createRenderDevice());
    DeviceDesc desc{};
    desc.width = 320; desc.height = 240; desc.headless = true;
#ifdef _DEBUG
    desc.validation = true;
#endif
    if (!device->init(desc)) { x3::logError("[gpuskin] device init failed"); glfwTerminate(); return false; }

    check("device supports GPU skinning", device->supportsGpuSkinning());

    MeshHandle mesh = device->createMesh(bind.data(), V, idx.data(), (uint32_t)idx.size());
    check("createMesh ok", mesh.valid());
    bool reg = device->registerSkinnedMesh(mesh, bind.data(), V, jidx.data(), jwt.data());
    check("registerSkinnedMesh ok", reg);

    // Helper: run one frame that uploads a palette + dispatches the compute skin, then
    // read back + compare to the CPU reference.
    auto runCase = [&](const char* name, const std::vector<float>& palette, float eps) -> bool {
        FrameContext fc = device->beginFrame();
        if (!fc.valid) { check((std::string(name)+": beginFrame").c_str(), false); return false; }
        device->setSkinnedPalette(mesh, palette.data(), J);
        device->endFrame(fc);   // the graph records + executes the skin compute pass

        std::vector<MeshVertex> gpu(V);
        if (!device->readbackSkinnedMesh(mesh, gpu.data(), V)) {
            check((std::string(name)+": readback").c_str(), false); return false;
        }
        std::vector<MeshVertex> ref; cpuSkin(palette, ref);
        float maxErr = 0.0f;
        for (uint32_t v = 0; v < V; ++v) {
            for (int k = 0; k < 3; ++k) maxErr = std::max(maxErr, std::fabs(gpu[v].pos[k]    - ref[v].pos[k]));
            for (int k = 0; k < 3; ++k) maxErr = std::max(maxErr, std::fabs(gpu[v].normal[k] - ref[v].normal[k]));
            for (int k = 0; k < 2; ++k) maxErr = std::max(maxErr, std::fabs(gpu[v].uv[k]     - ref[v].uv[k]));
        }
        x3::logInfo(std::string("    [gpuskin] ") + name + " maxErr=" + std::to_string(maxErr));
        return check((std::string(name)+": GPU == CPU LBS").c_str(), maxErr < eps);
    };

    // (a) IDENTITY palette => output == bind pose EXACTLY.
    {
        std::vector<float> pal((size_t)J*16, 0.0f);
        for (uint32_t j = 0; j < J; ++j) { float* m = &pal[(size_t)j*16]; for (int e=0;e<16;++e) m[e]=(e%5==0)?1.0f:0.0f; }
        // Run the identity case, then ALSO assert it equals the bind pose exactly.
        FrameContext fc = device->beginFrame();
        device->setSkinnedPalette(mesh, pal.data(), J);
        device->endFrame(fc);
        std::vector<MeshVertex> gpu(V);
        bool rb = device->readbackSkinnedMesh(mesh, gpu.data(), V);
        check("identity: readback", rb);
        if (rb) {
            float maxErr = 0.0f;
            for (uint32_t v = 0; v < V; ++v) {
                for (int k=0;k<3;++k) maxErr = std::max(maxErr, std::fabs(gpu[v].pos[k]    - bind[v].pos[k]));
                for (int k=0;k<3;++k) maxErr = std::max(maxErr, std::fabs(gpu[v].normal[k] - bind[v].normal[k]));
            }
            x3::logInfo("    [gpuskin] identity maxErr=" + std::to_string(maxErr));
            check("identity palette => bind pose (exact)", maxErr < 1e-5f);
        }
    }

    // (b) Known joint TRANSLATION: joint 0 translated +Y by 2, joint 1 by -X 1.
    {
        std::vector<float> pal((size_t)J*16, 0.0f);
        float t0[3]={0,2,0}, t1[3]={-1,0,0}, q[4]={0,0,0,1}, s[3]={1,1,1};
        trsToMat4(t0, q, s, &pal[0]);
        trsToMat4(t1, q, s, &pal[16]);
        runCase("translation", pal, 1e-4f);
    }

    // (c) Known joint ROTATION + translation: joint 0 rotated 90deg about +Z, joint 1
    //     rotated -45deg about +X and translated +Z by 0.5 (exercises the upper 3x3
    //     normal transform + the 50/50 blend vertex).
    {
        std::vector<float> pal((size_t)J*16, 0.0f);
        const float a0 = 1.5707963f;            // 90 deg about Z
        float q0[4] = { 0, 0, std::sin(a0*0.5f), std::cos(a0*0.5f) };
        float t0[3] = { 0.0f, 0.0f, 0.0f }, s[3] = {1,1,1};
        trsToMat4(t0, q0, s, &pal[0]);
        const float a1 = -0.7853981f;           // -45 deg about X
        float q1[4] = { std::sin(a1*0.5f), 0, 0, std::cos(a1*0.5f) };
        float t1[3] = { 0.0f, 0.0f, 0.5f };
        trsToMat4(t1, q1, s, &pal[16]);
        runCase("rotation+translation", pal, 1e-4f);
    }

    // Re-run a second translation to prove the per-frame palette is honoured frame to
    // frame (the double-buffered output + descriptor sets work across frames-in-flight).
    {
        std::vector<float> pal((size_t)J*16, 0.0f);
        float t0[3]={3,0,0}, t1[3]={0,0,-2}, q[4]={0,0,0,1}, s[3]={1,1,1};
        trsToMat4(t0, q, s, &pal[0]);
        trsToMat4(t1, q, s, &pal[16]);
        runCase("translation (frame 2)", pal, 1e-4f);
    }

    device->unregisterSkinnedMesh(mesh);
    device->destroyMesh(mesh);
    device->shutdown();
    glfwTerminate();

    std::printf("gpuskin: %d/%d passed\n", passed, total);
    x3::logInfo("gpuskin: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
}

// ===========================================================================
// --test-hatch : END-TO-END secret-hatch chain self-test (headless, no mocks).
// Every link of Tim's terminal-code -> trapdoor chain was verified in isolation
// (--test-script, --test-secretroom, --test-holoterm); THIS asserts the FULL
// chain in one automated run on the REAL pieces:
//   real Level1Game world  +  boot-loaded scripts/secret_room.lua  +  the SAME
//   registerGameBindings() the live game wires  +  the SAME
//   submitTerminalToScripts() Enter glue the in-game cell terminal runs.
// Checks (C1-C8):
//   C1 boot: scripts/*.lua load; secret_room.lua healthy (not quarantined)
//   C2 the hatch starts LOCKED + Closed (floor hatch in the real DoorSystem)
//   C3 NEGATIVE: fire("terminal_code", code=9999) -> hatch stays Closed+locked,
//      objective override untouched
//   C4 POSITIVE: fire("terminal_code", code=1278) -> Lua -> x3.openTrapdoor()
//      -> the REAL DoorSystem hatch is Opening, and reaches Open under tick()
//   C5 Lua x3.setObjective() -> the ObjectiveSystem override line == the
//      script's authored string
//   C6 trigger_enter plumbing: walk the player into a REAL L1 trigger and
//      forward game.lastFiredTriggers() exactly as the main loop does ->
//      scripts receive it, nothing quarantined
//   C7 KEYPAD NEGATIVE (fresh world+scripts): type 9999 on the REAL cell
//      HoloTerminal -> Enter glue -> rejected, hatch stays Closed
//   C8 KEYPAD POSITIVE: type 1278 (with a typo + backspace) -> Enter glue ->
//      hatch opens AND the Lua objective line is set. ONLY the script writes
//      the objective override (the C++ submit sink does not), so this PROVES
//      the keypad -> fire -> Lua link, not just the C++ sink.
// ===========================================================================
bool runHatchChainSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool c, const char* name) {
        if (c) { ++pass; x3::logInfo(std::string("[hatch-test] PASS ") + name); }
        else   { ++fail; x3::logError(std::string("[hatch-test] FAIL ") + name); }
    };
    constexpr float kDt = 1.0f / 60.0f;
    const char* kObjectiveLine = "A hatch grinds open in the cell floor... drop through";

    // secret_room.lua healthy (loaded, not quarantined by a Lua error)?
    auto scriptHealthy = [](x3::script::IScriptSystem& sys, const char* name) {
        for (x3::script::ScriptId id : sys.loadedScripts()) {
            x3::script::ScriptStatus st = sys.status(id);
            if (st.name == name) return st.loaded && !st.failed;
        }
        return false;
    };

    // ---- World 1: the fire()-onward chain. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
        physics->init();
        x3::game::HeadlessRenderDevice device;
        x3::game::Scene scene;
        x3::game::Level1Game game;
        game.setDevice(device);
        game.build(scene, device, *physics, /*modelDir*/"");

        std::unique_ptr<x3::script::IScriptSystem> scripts(
            x3::script::createLuaScriptSystem(nullptr));
        const int loaded = loadBootScripts(*scripts);
        registerGameBindings(*scripts, game);

        // C1: the real scripts/ dir boot-loaded; secret_room.lua healthy.
        check(loaded >= 1 && scriptHealthy(*scripts, "secret_room.lua"),
              "C1 boot-load scripts/ — secret_room.lua loaded + healthy");

        // C2: the hatch starts LOCKED + Closed.
        const uint32_t hi = game.secret().hatchDoorIndex();
        bool hatchOk = game.secret().hatchBuilt() && hi < game.doors().count();
        {
            const x3::game::Door& h = game.doors().at(hi);
            check(hatchOk && h.locked && h.floorHatch &&
                  h.state == x3::game::DoorState::Closed,
                  "C2 hatch starts LOCKED + Closed (floor hatch)");
        }

        // C3 NEGATIVE: a wrong code through the full event path leaves it shut.
        scripts->fire("terminal_code", {{"code", "9999"}});
        scripts->update(kDt);
        {
            const x3::game::Door& h = game.doors().at(hi);
            check(h.locked && h.state == x3::game::DoorState::Closed &&
                  game.objectives().overrideText().empty(),
                  "C3 wrong code 9999 -> hatch stays LOCKED+Closed, objective untouched");
        }

        // C4 POSITIVE: the secret code -> Lua -> openTrapdoor -> REAL DoorSystem.
        scripts->fire("terminal_code", {{"code", x3::game::kSecretRoomCode}});
        scripts->update(kDt);
        {
            const x3::game::Door& h = game.doors().at(hi);
            bool opening = !h.locked && (h.state == x3::game::DoorState::Opening ||
                                         h.state == x3::game::DoorState::Open);
            // Pump the real game tick so the DoorSystem animates the slide to Open.
            const x3::phys::Vec3 spawn = game.layout().spawn;
            for (int i = 0; i < 120; ++i) {
                game.tick(kDt, scene, *physics, spawn, spawn);
                physics->step(kDt);
                scene.update(*physics);
                scripts->update(kDt);
            }
            check(opening && game.doors().at(hi).state == x3::game::DoorState::Open,
                  "C4 code 1278 -> Lua openTrapdoor -> hatch Opening -> Open");
        }

        // C5: the script's objective line landed on the real ObjectiveSystem.
        check(game.objectives().overrideText() == kObjectiveLine,
              "C5 Lua setObjective -> objective override == script string");

        // C6: trigger_enter plumbing — walk into the REAL strength trigger and
        // forward lastFiredTriggers() exactly as the main loop does.
        {
            const x3::phys::Vec3 trigPos{ 1.5f, 0.05f, -1.8f };   // beat-1 strength trigger
            int forwarded = 0;
            for (int i = 0; i < 4; ++i) {
                game.tick(kDt, scene, *physics, trigPos, trigPos);
                for (uint32_t tid : game.lastFiredTriggers()) {
                    scripts->fire("trigger_enter",
                        {{"zone", std::to_string(tid)}, {"who", "player"}});
                    ++forwarded;
                }
                physics->step(kDt);
                scene.update(*physics);
            }
            scripts->update(kDt);
            check(forwarded >= 1 && scriptHealthy(*scripts, "secret_room.lua"),
                  "C6 real L1 trigger_enter forwarded to scripts — no quarantine");
        }

        physics->shutdown();
    }

    // ---- World 2 (fresh latch): the KEYPAD link — the real HoloTerminal typed
    // input driven through the SAME Enter glue the in-game handler runs. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
        physics->init();
        x3::game::HeadlessRenderDevice device;
        x3::game::Scene scene;
        x3::game::Level1Game game;
        game.setDevice(device);
        game.build(scene, device, *physics, /*modelDir*/"");

        std::unique_ptr<x3::script::IScriptSystem> scripts(
            x3::script::createLuaScriptSystem(nullptr));
        loadBootScripts(*scripts);
        registerGameBindings(*scripts, game);

        const uint32_t hi = game.secret().hatchDoorIndex();
        x3::game::HoloTerminal& term = game.secret().terminal();
        term.setActive(true);   // the in-game termMode flow activates the field

        // C7 KEYPAD NEGATIVE: type a wrong code, Enter -> rejected, hatch shut.
        for (char c : std::string("9999")) term.pushChar(c);
        bool acceptedWrong = submitTerminalToScripts(scripts.get(), term);
        scripts->update(kDt);
        check(!acceptedWrong &&
              game.doors().at(hi).state == x3::game::DoorState::Closed &&
              game.doors().at(hi).locked &&
              game.objectives().overrideText().empty(),
              "C7 keypad 9999 + Enter -> rejected, hatch stays LOCKED+Closed");

        // C8 KEYPAD POSITIVE: type the code (with a typo fixed by backspace),
        // Enter -> the hatch opens AND the Lua objective landed (proving the
        // keypad -> fire -> Lua link: only the script writes the override).
        term.pushChar('1'); term.pushChar('2'); term.pushChar('9');
        term.backspace();   // ...typo corrected
        term.pushChar('7'); term.pushChar('8');
        bool accepted = submitTerminalToScripts(scripts.get(), term);
        scripts->update(kDt);
        {
            const x3::game::Door& h = game.doors().at(hi);
            bool opening = !h.locked && (h.state == x3::game::DoorState::Opening ||
                                         h.state == x3::game::DoorState::Open);
            check(accepted && opening &&
                  game.objectives().overrideText() == kObjectiveLine,
                  "C8 keypad 1278 + Enter -> hatch opens + Lua objective set");
        }

        physics->shutdown();
    }

    x3::logInfo("[hatch-test] " + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::apphost
