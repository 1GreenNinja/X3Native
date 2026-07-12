// ============================================================================
// host_bodycontact — eyes-on test host for the BodyContact engine feature
// (engine/physics/BodyContact.*): a posed synthetic body (supine, knees up —
// the F2 captive pose) resting on (LEFT) a RIGID steel slab and (RIGHT) a
// tessellated SOFT mattress that visibly DENTS under the body's weight.
//
//   * WALKABLE-ish (windowed): --world bodycontact — slow orbit cam, Esc quits.
//   * SCREENSHOT (headless): --world bodycontact --screenshot [--shot-cam ...]
//     captures/bodycontact_rigid.png / _soft.png are shot with two --shot-cam
//     runs (the host prints both suggested cams at boot).
//
// The solve + indent run ONCE at build (static staging — exactly how the F2
// rescue captives consume it); the per-frame path is the same pure calls.
// ============================================================================

#include "../world_hosts.h"
#include "../host_context.h"
#include "../mesh_prims.h"
#include "engine/physics/BodyContact.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/core/x3_log.h"

#include <GLFW/glfw3.h>
#include <cmath>
#include <string>
#include <vector>

namespace x3 { namespace apphost {

namespace {

// Column-major uniform scale + translate.
void modelTS(float s, float tx, float ty, float tz, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0] = out[5] = out[10] = s; out[15] = 1.0f;
    out[12] = tx; out[13] = ty; out[14] = tz;
}

// The synthetic supine body: a small bone tree (head->neck->chest->pelvis,
// arms at the sides, KNEES UP with heels on the surface — the captive pose).
// Positions are body-local: +X toward the feet, y = surface-relative height.
std::vector<x3::phys::ContactBone> makeSupineBody(float ox, float oy, float oz) {
    using x3::phys::ContactBone;
    auto B = [&](int parent, float x, float y, float z, float r, float m) {
        ContactBone b; b.parent = parent;
        b.pos = { ox + x, oy + y, oz + z }; b.radius = r; b.massFrac = m;
        return b;
    };
    std::vector<ContactBone> bones;
    bones.push_back(B(-1, -0.78f, 0.045f,  0.00f, 0.090f, 0.08f));  // 0 head
    bones.push_back(B( 0, -0.62f, 0.040f,  0.00f, 0.060f, 0.05f));  // 1 neck
    bones.push_back(B( 1, -0.42f, 0.045f,  0.00f, 0.110f, 0.18f));  // 2 chest
    bones.push_back(B( 2, -0.10f, 0.040f,  0.00f, 0.110f, 0.22f));  // 3 pelvis
    bones.push_back(B( 2, -0.40f, 0.030f, -0.20f, 0.050f, 0.04f));  // 4 elbow L
    bones.push_back(B( 2, -0.40f, 0.030f,  0.20f, 0.050f, 0.04f));  // 5 elbow R
    bones.push_back(B( 4, -0.15f, 0.025f, -0.22f, 0.040f, 0.03f));  // 6 hand L
    bones.push_back(B( 5, -0.15f, 0.025f,  0.22f, 0.040f, 0.03f));  // 7 hand R
    // Knees UP (raised well above the surface), legs SPREAD, heels down.
    bones.push_back(B( 3,  0.22f, 0.280f, -0.16f, 0.060f, 0.05f));  // 8 knee L
    bones.push_back(B( 3,  0.22f, 0.280f,  0.16f, 0.060f, 0.05f));  // 9 knee R
    bones.push_back(B( 8,  0.46f, 0.030f, -0.19f, 0.050f, 0.065f)); // 10 heel L
    bones.push_back(B( 9,  0.46f, 0.030f,  0.19f, 0.050f, 0.065f)); // 11 heel R
    return bones;
}

} // namespace

int hostBodyContact(HostContext& hc) {
    if (hc.worldMode != "bodycontact") return -1;
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    x3::logInfo("--world bodycontact: BODY CONTACT feature host (rigid rest + soft mattress indent)");
    x3::logInfo("  suggested cams: RIGID --shot-cam \"-2.0,1.75,2.3,-1.5708,-0.42\"  "
                "SOFT --shot-cam \"2.0,1.75,2.3,-1.5708,-0.42\"");

    // ---- Look: interior test cell (no sky; SSAO/GI off per standalone law) ----
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
    { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
    {   // Raking key low across the mattress (the dent must READ) + cool fill.
        x3::rhi::PointLight pl[3]{};
        pl[0].pos[0] = 2.0f;  pl[0].pos[1] = 0.85f; pl[0].pos[2] = -1.6f; pl[0].range = 7.0f;
        pl[0].color[0] = 3.4f; pl[0].color[1] = 3.1f; pl[0].color[2] = 2.6f;
        pl[1].pos[0] = -2.0f; pl[1].pos[1] = 0.95f; pl[1].pos[2] = -1.6f; pl[1].range = 7.0f;
        pl[1].color[0] = 3.0f; pl[1].color[1] = 3.0f; pl[1].color[2] = 3.2f;
        pl[2].pos[0] = 0.0f;  pl[2].pos[1] = 2.8f;  pl[2].pos[2] = 1.8f;  pl[2].range = 14.0f;
        pl[2].color[0] = 1.1f; pl[2].color[1] = 1.15f; pl[2].color[2] = 1.3f;
        device->setPointLights(pl, 3);
    }

    // ---- Static geometry ------------------------------------------------
    struct Draw { x3::rhi::MeshHandle mesh; float tint[4]; float model[16]; };
    std::vector<Draw> draws;
    auto addBox = [&](float hx, float hy, float hz, float cx, float cy, float cz,
                      float r, float g, float b) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 1.0f);
        Draw d{};
        d.mesh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                    m.index.data(), (uint32_t)m.index.size());
        d.tint[0] = r; d.tint[1] = g; d.tint[2] = b; d.tint[3] = 1.0f;
        modelTS(1.0f, 0, 0, 0, d.model);
        draws.push_back(d);
    };
    // Floor + the two platforms.
    addBox(4.6f, 0.06f, 2.6f, 0.0f, -0.06f, 0.0f, 0.32f, 0.34f, 0.38f);   // floor
    addBox(1.15f, 0.35f, 0.62f, -2.0f, 0.35f, 0.0f, 0.30f, 0.31f, 0.35f); // rigid steel slab (top 0.70)
    addBox(1.10f, 0.25f, 0.58f,  2.0f, 0.25f, 0.0f, 0.28f, 0.26f, 0.24f); // bed base (top 0.50)
    addBox(1.05f, 0.055f, 0.52f, 2.0f, 0.555f, 0.0f, 0.85f, 0.82f, 0.74f);// mattress side skirt (top 0.61)

    // ---- The SOFT mattress patch (tessellated; this is what dents) -------
    const uint32_t NX = 49, NZ = 25;
    const float MW = 1.02f, MD = 0.50f, MY = 0.615f;   // half-extents + top Y
    std::vector<x3::rhi::MeshVertex> mverts((size_t)NX * NZ);
    std::vector<uint32_t> midx;
    for (uint32_t z = 0; z < NZ; ++z)
        for (uint32_t x = 0; x < NX; ++x) {
            x3::rhi::MeshVertex& v = mverts[(size_t)z * NX + x];
            v.pos[0] = 2.0f + MW * (2.0f * (float)x / (float)(NX - 1) - 1.0f);
            v.pos[1] = MY;
            v.pos[2] = MD * (2.0f * (float)z / (float)(NZ - 1) - 1.0f);
            v.normal[0] = 0; v.normal[1] = 1; v.normal[2] = 0;
            v.uv[0] = (float)x / (float)(NX - 1); v.uv[1] = (float)z / (float)(NZ - 1);
        }
    for (uint32_t z = 0; z + 1 < NZ; ++z)
        for (uint32_t x = 0; x + 1 < NX; ++x) {
            const uint32_t a = z * NX + x, b = a + 1, c = a + NX, d = c + 1;
            midx.insert(midx.end(), { a, c, b,  b, c, d });
        }

    // ---- Surfaces + the two bodies ---------------------------------------
    using x3::phys::ContactSurface;
    ContactSurface floorS{};                                   // rigid, infinite
    ContactSurface slabTop{};                                  // rigid, finite
    slabTop.point = { -2.0f, 0.70f, 0.0f }; slabTop.halfU = 1.15f; slabTop.halfV = 0.62f;
    ContactSurface mattress{};                                 // SOFT, finite
    mattress.point = { 2.0f, MY, 0.0f }; mattress.halfU = MW; mattress.halfV = MD;
    mattress.soft = true; mattress.indentBudget = 0.06f;

    // Bodies start slightly SUNKEN so the solve visibly does its job.
    auto rigidBody = makeSupineBody(-2.0f, 0.70f - 0.02f, 0.0f);
    auto softBody  = makeSupineBody( 2.0f, MY   - 0.02f, 0.0f);
    {
        const ContactSurface rigidSet[2] = { floorS, slabTop };
        const x3::phys::BodySolveStats s1 =
            x3::phys::solveBodyContact(rigidBody.data(), (uint32_t)rigidBody.size(), rigidSet, 2);
        const ContactSurface softSet[2] = { floorS, mattress };
        const x3::phys::BodySolveStats s2 =
            x3::phys::solveBodyContact(softBody.data(), (uint32_t)softBody.size(), softSet, 2);
        x3::logInfo("[bodycontact] solve: rigid resolved=" + std::to_string(s1.contactsResolved) +
                    " residual=" + std::to_string(s1.maxRigidPenetration) +
                    " | soft resolved=" + std::to_string(s2.contactsResolved) +
                    " softDepth=" + std::to_string(s2.maxSoftPenetration));
        // Bake the dent into the mattress (static staging path).
        x3::phys::IndentParams ip{}; ip.falloffRadius = 0.26f;
        const uint32_t bent = x3::phys::bakeSoftIndentation(
            softBody.data(), (uint32_t)softBody.size(), mattress,
            &mverts[0].pos[0], (uint32_t)mverts.size(),
            sizeof(x3::rhi::MeshVertex) / sizeof(float), 3,
            midx.data(), (uint32_t)midx.size(), ip);
        x3::logInfo("[bodycontact] indent baked: " + std::to_string(bent) + " vert(s) displaced");
    }
    Draw mattressDraw{};
    mattressDraw.mesh = device->createMesh(mverts.data(), (uint32_t)mverts.size(),
                                           midx.data(), (uint32_t)midx.size());
    mattressDraw.tint[0] = 0.92f; mattressDraw.tint[1] = 0.88f; mattressDraw.tint[2] = 0.78f;
    mattressDraw.tint[3] = 1.0f;
    modelTS(1.0f, 0, 0, 0, mattressDraw.model);
    draws.push_back(mattressDraw);

    // Bone spheres (shared unit sphere, scaled per bone).
    x3::prims::PrimMesh sph = x3::prims::makeUVSphere(24, 32);
    x3::rhi::MeshHandle sphMesh = device->createMesh(sph.verts.data(), (uint32_t)sph.verts.size(),
                                                     sph.index.data(), (uint32_t)sph.index.size());
    auto drawBody = [&](const x3::rhi::FrameContext& frame,
                        const std::vector<x3::phys::ContactBone>& body,
                        float r, float g, float b) {
        for (const auto& bn : body) {
            float mm[16]; modelTS(bn.radius, bn.pos.x, bn.pos.y, bn.pos.z, mm);
            const float tint[4] = { r, g, b, 1.0f };
            const float emis[4] = { 0, 0, 0, 0 };
            device->drawMeshPBR(frame, sphMesh, x3::rhi::TextureHandle{},
                                x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                tint, emis, mm);
        }
    };
    auto drawScene = [&](const x3::rhi::FrameContext& frame) {
        const float emis[4] = { 0, 0, 0, 0 };
        for (const auto& d : draws)
            device->drawMeshPBR(frame, d.mesh, x3::rhi::TextureHandle{},
                                x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                d.tint, emis, d.model);
        drawBody(frame, rigidBody, 0.45f, 0.50f, 0.60f);
        drawBody(frame, softBody,  0.60f, 0.48f, 0.42f);
    };

    // ===== Headless screenshot =====
    if (hc.headless) {
        float cam[5] = { 2.0f, 1.75f, 2.3f, -1.5708f, -0.42f };   // default: the SOFT bed
        if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("captures/bodycontact.png");
        const int kSettle = 8;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 62.0f);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world bodycontact: wrote " + outPath);
        else       x3::logError("--world bodycontact: capture FAILED");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Windowed: slow orbit around the soft bed =====
    double t0 = glfwGetTime();
    int lastW = 0, lastH = 0;
    glfwGetFramebufferSize(window, &lastW, &lastH);
    x3::logInfo("--world bodycontact: orbiting; Esc to quit");
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        const float t = (float)(glfwGetTime() - t0);
        const float yaw = -1.5708f + 0.35f * std::sin(t * 0.25f);
        const float cx = 0.0f - 3.2f * std::cos(yaw), cz = 0.0f - 3.2f * std::sin(yaw);
        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) { lastW = cw; lastH = ch; if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch); }
        device->setCamera(cx, 1.8f, cz, yaw, -0.40f, 62.0f);
        auto frame = device->beginFrame();
        if (frame.valid) drawScene(frame);
        device->endFrame(frame);
    }
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
