// Vehicle demo worlds — implementation. See vehicle.h.
//
// Built only through the public engine interfaces (IRenderDevice / IPhysicsWorld /
// IVehicleController). Clean-room.

#include "vehicle.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstring>
#include <string>

namespace x3::game {

namespace {

// Build a column-major 4x4 model matrix from a position + quaternion (x,y,z,w)
// + per-axis scale. RH, matches CONVENTIONS.md (the renderer/Scene use the same
// column-major glm layout, translation in m[12..14]).
void composeTRS(const float pos[3], const float q[4],
                float sx, float sy, float sz, float out[16]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x*x, yy = y*y, zz = z*z;
    const float xy = x*y, xz = x*z, yz = y*z;
    const float wx = w*x, wy = w*y, wz = w*z;
    // Rotation matrix (column-major), columns scaled.
    out[0]  = (1.0f - 2.0f*(yy+zz)) * sx;
    out[1]  = (2.0f*(xy+wz))        * sx;
    out[2]  = (2.0f*(xz-wy))        * sx;
    out[3]  = 0.0f;
    out[4]  = (2.0f*(xy-wz))        * sy;
    out[5]  = (1.0f - 2.0f*(xx+zz)) * sy;
    out[6]  = (2.0f*(yz+wx))        * sy;
    out[7]  = 0.0f;
    out[8]  = (2.0f*(xz+wy))        * sz;
    out[9]  = (2.0f*(yz-wx))        * sz;
    out[10] = (1.0f - 2.0f*(xx+yy)) * sz;
    out[11] = 0.0f;
    out[12] = pos[0]; out[13] = pos[1]; out[14] = pos[2]; out[15] = 1.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// Unit Y-cylinder (radius 1, height 1 -> y in [-0.5, 0.5]).
// ---------------------------------------------------------------------------
void makeUnitCylinderY(uint32_t segments,
                       std::vector<x3::rhi::MeshVertex>& verts,
                       std::vector<uint32_t>& idx) {
    verts.clear(); idx.clear();
    if (segments < 6) segments = 6;
    const float hy = 0.5f;
    // Side ring vertices (top + bottom), with outward normals.
    for (uint32_t i = 0; i <= segments; ++i) {
        float a = (float)i / (float)segments * 6.2831853f;
        float cx = std::cos(a), cz = std::sin(a);
        float u = (float)i / (float)segments;
        verts.push_back({{cx, -hy, cz}, {cx, 0.0f, cz}, {u, 0.0f}}); // bottom
        verts.push_back({{cx,  hy, cz}, {cx, 0.0f, cz}, {u, 1.0f}}); // top
    }
    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t b = i * 2;
        // two triangles per quad (CCW for VK counter-clockwise front face)
        idx.insert(idx.end(), { b, b+1, b+2,  b+2, b+1, b+3 });
    }
    // End caps (center fan).
    uint32_t topCenter = (uint32_t)verts.size();
    verts.push_back({{0,  hy, 0}, {0, 1, 0}, {0.5f, 0.5f}});
    uint32_t botCenter = (uint32_t)verts.size();
    verts.push_back({{0, -hy, 0}, {0,-1, 0}, {0.5f, 0.5f}});
    for (uint32_t i = 0; i < segments; ++i) {
        float a0 = (float)i / (float)segments * 6.2831853f;
        float a1 = (float)(i+1) / (float)segments * 6.2831853f;
        float c0x=std::cos(a0), c0z=std::sin(a0), c1x=std::cos(a1), c1z=std::sin(a1);
        uint32_t t0 = (uint32_t)verts.size();
        verts.push_back({{c0x, hy, c0z}, {0,1,0}, {0,0}});
        verts.push_back({{c1x, hy, c1z}, {0,1,0}, {1,0}});
        idx.insert(idx.end(), { topCenter, t0, t0+1 });
        uint32_t bb = (uint32_t)verts.size();
        verts.push_back({{c0x,-hy,c0z}, {0,-1,0}, {0,0}});
        verts.push_back({{c1x,-hy,c1z}, {0,-1,0}, {1,0}});
        idx.insert(idx.end(), { botCenter, bb+1, bb });
    }
}

// ===========================================================================
// DriveDemo
// ===========================================================================
bool DriveDemo::buildPhysics(x3::phys::IPhysicsWorld& physics, float x, float y, float z) {
    m_physics = &physics;

    // --- Chassis dynamic body (a box). Layer Dynamic. ---
    m_chassis = physics.addBox(x3::phys::Vec3{m_hx, m_hy, m_hz},
                               x3::phys::Vec3{x, y, z}, 1300.0f, x3::phys::Layer::Dynamic);
    if (!m_chassis.valid()) return false;

    // --- 4 wheels at the HERO-CAR GLB stations (CTR, after the nose flip to the
    // engine's -Z forward: fronts z=-1.186, rears z=+1.088, track +-0.677 m).
    // Front steered, rear powered + handbrake — grippy arcade RWD. ---
    m_wheels.clear();
    struct P { float wx, wz; bool steer, hb; bool powered; };
    P p[4] = {
        { -0.677f, -1.186f, true,  false, false },   // front-left
        {  0.677f, -1.186f, true,  false, false },   // front-right
        { -0.723f,  1.088f, false, true,  true  },   // rear-left  (drive; wider track)
        {  0.723f,  1.088f, false, true,  true  },   // rear-right (drive)
    };
    for (int i = 0; i < 4; ++i) {
        x3::phys::WheelDesc w;
        // Attach high in the wheel well (NOT the box bottom) so the rest pose
        // matches the GLB arches: wheel center = attach - suspension (~0.30 m).
        w.position[0] = p[i].wx; w.position[1] = -0.15f; w.position[2] = p[i].wz;
        w.radius = 0.33f; w.width = 0.24f;
        w.suspensionMin = 0.15f; w.suspensionMax = 0.42f;
        w.suspensionFreq = 2.2f; w.suspensionDamp = 0.7f;
        w.steered = p[i].steer; w.handBraked = p[i].hb; w.powered = p[i].powered;
        w.maxSteerAngle = 0.5236f; // ~30deg
        w.maxBrakeTorque = 2200.0f;
        m_wheels.push_back(w);
    }
    x3::phys::WheeledVehicleDesc vd;
    vd.chassis = m_chassis;
    vd.wheels = m_wheels.data(); vd.wheelCount = (uint32_t)m_wheels.size();
    vd.maxEngineTorque = 700.0f; vd.maxEngineRPM = 6500.0f;
    // Wheel rays cast as Dynamic so they stand on the Static terrain (Static-vs-
    // Static doesn't collide in the engine matrix; Dynamic-vs-Static does).
    vd.groundLayer = x3::phys::Layer::Dynamic;
    m_ctl.reset(x3::phys::createWheeledVehicle(physics, vd));
    if (!m_ctl) { physics.removeBody(m_chassis); m_chassis = {}; return false; }
    return true;
}

bool DriveDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                      float x, float y, float z) {
    m_device = &device;
    if (!buildPhysics(physics, x, y, z)) return false;

    // --- Render meshes ---
    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);  // unit cube; we scale per draw
    m_chassisMesh = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    std::vector<x3::rhi::MeshVertex> wv; std::vector<uint32_t> wi;
    makeUnitCylinderY(16, wv, wi);
    m_wheelMesh = device.createMesh(wv.data(), (uint32_t)wv.size(), wi.data(), (uint32_t)wi.size());

    auto cTex = x3::prims::makeSolidRGBA(8, 200, 40, 40);    // red car body
    m_chassisTex = device.createTexture(cTex.data(), 8, 8, true);
    auto wTex = x3::prims::makeSolidRGBA(8, 30, 30, 35);     // dark tires
    m_wheelTex = device.createTexture(wTex.data(), 8, 8, true);
    return true;
}

// ---------------------------------------------------------------------------
// HERO-CAR GLB skin. Partition the converted GLB's drawables by node name:
// Wheel_FL/FR/RL/RR follow the live physics wheel poses; everything else is the
// sprung body. The GLB is authored nose=+Z, origin on the ground plane; the
// engine car drives nose=-Z with the physics chassis center ~0.76 m above the
// ground at rest — both baked into kBodySkin below.
// ---------------------------------------------------------------------------
namespace {
// GLB ground-origin -> physics chassis-center offset + the 180-deg nose flip.
// (chassis center = wheel attach (-0.15) + rest suspension (~0.28) + wheel
// radius 0.33 above the ground plane => drop the skin by the sum.)
constexpr float kBodyDropY = -0.76f;
const float kBodySkin[16] = { -1,0,0,0,  0,1,0,0,  0,0,-1,0,  0,kBodyDropY,0,1 };
// Mesh-local wheel axis is +-X (car lateral); the physics wheel pose maps a unit
// Y-cylinder (axis = axle). Rotate mesh X onto pose Y (Rz +90deg, column-major).
const float kWheelAxisFix[16] = { 0,1,0,0,  -1,0,0,0,  0,0,1,0,  0,0,0,1 };
} // namespace

bool DriveDemo::skin(x3::rhi::IRenderDevice& device, std::string_view glbDir,
                     std::string_view relPath) {
    m_skinSrc.reset(x3::asset::createAssetSource());
    if (!m_skinSrc || !m_skinSrc->mountDir(glbDir, 0)) return false;
    m_skinLoader.reset(x3::asset::createModelLoader(&device, m_skinSrc.get()));
    m_skinModel = m_skinLoader->load(relPath);
    if (!m_skinModel.ok) return false;

    std::vector<std::string> names;
    std::vector<x3::asset::ModelDrawable> all = x3::asset::makeDrawablesNamed(m_skinModel, names);
    // GLB wheel name -> physics wheel slot. The nose flip maps GLB FL->engine FL
    // (GLB +X/+Z both negate, so left/right and front/rear BOTH swap = identity).
    auto slotOf = [](const std::string& nm) -> int {
        if (nm.find("Wheel_FL") != std::string::npos) return 0;
        if (nm.find("Wheel_FR") != std::string::npos) return 1;
        if (nm.find("Wheel_RL") != std::string::npos) return 2;
        if (nm.find("Wheel_RR") != std::string::npos) return 3;
        return -1;
    };
    m_bodyDraw.clear();
    for (int s = 0; s < 4; ++s) m_wheelDraw[s].clear();
    for (size_t i = 0; i < all.size(); ++i) {
        const int s = (i < names.size()) ? slotOf(names[i]) : -1;
        if (s < 0) { m_bodyDraw.push_back(all[i]); continue; }
        // Wheel drawable: bake (axis fix) * (node transform WITHOUT translation —
        // the physics pose supplies position/steer/spin; keep authored scale).
        x3::asset::ModelDrawable d = all[i];
        float noT[16]; std::memcpy(noT, d.nodeTransform, sizeof(noT));
        noT[12] = noT[13] = noT[14] = 0.0f;
        float local[16];
        x3::asset::mulMat4(kWheelAxisFix, noT, local);
        std::memcpy(d.nodeTransform, local, sizeof(local));
        m_wheelDraw[s].push_back(d);
    }
    m_skinned = !m_bodyDraw.empty();
    return m_skinned;
}

void DriveDemo::drawDrawable(const x3::rhi::FrameContext& f,
                             const x3::asset::ModelDrawable& d, const float world[16]) const {
    const bool matEmis = d.emissiveTexId != 0 ||
        d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f || d.emissiveFactor[2] > 0.001f;
    float emis[4] = { d.emissiveFactor[0], d.emissiveFactor[1], d.emissiveFactor[2],
                      matEmis ? 1.0f : 0.0f };
    m_device->drawMeshPBR(f,
                          x3::rhi::MeshHandle{ d.meshId },
                          x3::rhi::TextureHandle{ d.baseColorTexId },
                          x3::rhi::TextureHandle{ d.normalTexId },
                          x3::rhi::TextureHandle{ d.mrTexId },
                          d.baseColorFactor, emis, world,
                          d.alphaMask, d.alphaBlend,
                          x3::rhi::TextureHandle{ d.emissiveTexId },
                          x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
                          d.clearcoat, d.clearcoatRough);   // car-paint clearcoat lobe
}

bool DriveDemo::allWheelsInContact() const {
    if (!m_ctl) return false;
    const uint32_t n = m_ctl->wheelCount();
    for (uint32_t i = 0; i < n; ++i) {
        x3::phys::WheelState ws;
        if (!m_ctl->wheelState(i, ws) || !ws.hasContact) return false;
    }
    return n > 0;
}

void DriveDemo::setInput(const x3::phys::VehicleInput& in) { if (m_ctl) m_ctl->setInput(in); }
void DriveDemo::preStep(float dt)  { if (m_ctl) m_ctl->preStep(dt); }
void DriveDemo::postStep(float dt) { if (m_ctl) m_ctl->postStep(dt); }

void DriveDemo::chassisPos(float out[3]) const {
    x3::phys::Vec3 p = m_physics ? m_physics->getBodyPosition(m_chassis) : x3::phys::Vec3{};
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}

void DriveDemo::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device || !m_ctl) return;

    x3::phys::Vec3 p = m_physics->getBodyPosition(m_chassis);
    float q[4]; m_physics->getBodyRotation(m_chassis, q);
    float pos[3] = { p.x, p.y, p.z };

    if (m_skinned) {
        // ---- HERO-CAR GLB skin: the body parts ride the sprung chassis (nose
        // flip + ride-height drop baked in kBodySkin); the wheels ride the LIVE
        // physics wheel poses (steer + spin + suspension travel). ----
        float chassisM[16]; composeTRS(pos, q, 1.0f, 1.0f, 1.0f, chassisM);
        float carM[16];     x3::asset::mulMat4(chassisM, kBodySkin, carM);
        float fin[16];
        for (const auto& d : m_bodyDraw) {
            x3::asset::mulMat4(carM, d.nodeTransform, fin);
            drawDrawable(frame, d, fin);
        }
        for (int s = 0; s < 4; ++s) {
            x3::phys::WheelState ws;
            if (!m_ctl->wheelState((uint32_t)s, ws)) continue;
            // Strip the baked radius/half-width scale -> the pure wheel POSE.
            float P[16]; std::memcpy(P, ws.worldTransform, sizeof(P));
            for (int c = 0; c < 3; ++c) {
                float* col = &P[c * 4];
                const float len = std::sqrt(col[0]*col[0] + col[1]*col[1] + col[2]*col[2]);
                if (len > 1e-5f) { col[0] /= len; col[1] /= len; col[2] /= len; }
            }
            for (const auto& d : m_wheelDraw[s]) {
                x3::asset::mulMat4(P, d.nodeTransform, fin);   // nodeTransform = axisFix * authored scale
                drawDrawable(frame, d, fin);
            }
        }
        return;
    }

    // ---- Graybox fallback (no GLB): box chassis + cylinder wheels. ----
    const float bodyCol[4]  = { 1.0f, 0.25f, 0.22f, 1.0f };
    const float wheelCol[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
    float m[16]; composeTRS(pos, q, m_hx*2.0f, m_hy*2.0f, m_hz*2.0f, m);
    m_device->drawMesh(frame, m_chassisMesh, m_chassisTex, bodyCol, m);
    const uint32_t n = m_ctl->wheelCount();
    for (uint32_t i = 0; i < n; ++i) {
        x3::phys::WheelState ws;
        if (!m_ctl->wheelState(i, ws)) continue;
        m_device->drawMesh(frame, m_wheelMesh, m_wheelTex, wheelCol, ws.worldTransform);
    }
}

void DriveDemo::shutdown() {
    m_ctl.reset();  // remove the constraint/step-listener BEFORE the body/world go
    if (m_physics && m_chassis.valid()) m_physics->removeBody(m_chassis);
    if (m_device) {
        if (m_chassisMesh.valid()) m_device->destroyMesh(m_chassisMesh);
        if (m_wheelMesh.valid())   m_device->destroyMesh(m_wheelMesh);
        if (m_chassisTex.valid())  m_device->destroyTexture(m_chassisTex);
        if (m_wheelTex.valid())    m_device->destroyTexture(m_wheelTex);
    }
    // GLB skin: the loader frees the model's GPU handles (meshes/textures).
    if (m_skinned && m_skinLoader) m_skinLoader->unload(m_skinModel);
    m_bodyDraw.clear();
    for (int s = 0; s < 4; ++s) m_wheelDraw[s].clear();
    m_skinned = false;
    m_skinLoader.reset();
    m_skinSrc.reset();
    m_device = nullptr; m_physics = nullptr;
}

// ===========================================================================
// Headless DRIVE enter/exit self-test (--test-vehicle, game layer). Physics
// only — no render device. Mirrors the in-world UX: walk up, E to enter, drive,
// E to exit (control restored beside the car).
// ===========================================================================
bool runDriveEnterExitSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++passN; x3::logInfo(std::string("[drive-test] PASS ") + name); }
        else    { ++failN; x3::logError(std::string("[drive-test] FAIL ") + name); }
    };

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) { x3::logError("[drive-test] physics init failed"); return false; }
    {
        // Flat static slab the wheel rays can stand on.
        x3::prims::PrimMesh g = x3::prims::makeBox(200.0f, 0.5f, 200.0f, 0.0f, -0.5f, 0.0f, 0.02f);
        phys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                            g.cindex.data(), (uint32_t)g.cindex.size());
    }
    DriveDemo car;
    check(car.buildPhysics(*phys, 0.0f, 1.2f, 0.0f), "spawn: chassis + wheeled controller built");
    phys->optimizeBroadphase();

    const float dt = 1.0f / 60.0f;
    // Player ON FOOT beside the spawn point.
    float player[3] = { 3.0f, 0.0f, 4.0f };
    bool inCar = false;

    // Settle onto the suspension.
    for (int i = 0; i < 90; ++i) {
        x3::phys::VehicleInput in{};
        car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
    }
    check(car.allWheelsInContact(), "settle: all 4 wheels in ground contact");

    // 'E' — proximity enter (the in-world rule: within 3.5 m of the chassis).
    float c0[3]; car.chassisPos(c0);
    const float dEnter = std::sqrt((player[0]-c0[0])*(player[0]-c0[0]) +
                                   (player[2]-c0[2])*(player[2]-c0[2]));
    if (dEnter <= 5.0f) inCar = true;
    check(inCar, "enter: player within range takes the wheel");

    // Full throttle for 240 fixed ticks (4 s).
    for (int i = 0; i < 240; ++i) {
        x3::phys::VehicleInput in{};
        in.throttle = 1.0f;
        car.setInput(in); car.preStep(dt); phys->step(dt); car.postStep(dt);
    }
    float c1[3]; car.chassisPos(c1);
    const float dx = c1[0] - c0[0], dz = c1[2] - c0[2];
    const float disp = std::sqrt(dx*dx + dz*dz);
    x3::logInfo("[drive-test] displacement after 4 s full throttle: " + std::to_string(disp) +
                " m, fwdSpeed=" + std::to_string(car.forwardSpeed()) + " m/s");
    check(disp > 10.0f, "drive: forward displacement > 10 m");
    check(dz < -5.0f, "drive: displacement is along -Z (the car's forward)");
    check(car.forwardSpeed() > 3.0f, "drive: forward speed positive");
    check(car.allWheelsInContact(), "drive: wheels kept ground contact");

    // 'E' — exit: control restored ON FOOT beside the car.
    inCar = false;
    player[0] = c1[0] + 2.5f; player[1] = c1[1]; player[2] = c1[2];
    const float dExit = std::sqrt((player[0]-c1[0])*(player[0]-c1[0]) +
                                  (player[2]-c1[2])*(player[2]-c1[2]));
    check(!inCar && dExit > 2.0f && dExit < 3.0f, "exit: player control restored beside the car");

    car.shutdown();
    phys->shutdown();
    x3::logInfo("[drive-test] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

// ===========================================================================
// BoatDemo
// ===========================================================================
bool BoatDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                     float x, float y, float z, float seaLevel, bool isSub) {
    m_device = &device; m_physics = &physics;

    // Hull mass tuned so the box rides ~half-submerged in sea water:
    //   equilibrium submergedVol = mass / fluidDensity. For ~half of fullVol
    //   (=8*hx*hy*hz) submerged we want mass ~= 0.5 * fullVol * fluidDensity.
    const float fullVol = 8.0f * m_hx * m_hy * m_hz;
    const float fluidDensity = 1025.0f;
    const float mass = 0.5f * fullVol * fluidDensity * 0.95f; // slight float-high bias
    m_hull = physics.addBox(x3::phys::Vec3{m_hx, m_hy, m_hz},
                            x3::phys::Vec3{x, y, z}, mass, x3::phys::Layer::Dynamic);
    if (!m_hull.valid()) return false;

    x3::phys::BuoyancyDesc bd;
    bd.body = m_hull; bd.seaLevel = seaLevel;
    bd.halfExtents[0]=m_hx; bd.halfExtents[1]=m_hy; bd.halfExtents[2]=m_hz;
    bd.fluidDensity = fluidDensity;
    bd.linearDrag = 2.5f; bd.angularDrag = 2.5f;
    bd.propThrust = mass * 4.0f;     // can motor forward
    bd.steerTorque = mass * 1.5f;    // and turn
    if (isSub) bd.diveThrust = mass * 12.0f; // strong enough to submerge
    m_ctl.reset(x3::phys::createBuoyancyController(physics, bd));
    if (!m_ctl) { physics.removeBody(m_hull); m_hull = {}; return false; }

    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);
    m_hullMesh = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    auto t = isSub ? x3::prims::makeSolidRGBA(8, 180, 180, 60)   // yellow sub
                   : x3::prims::makeSolidRGBA(8, 150, 90, 50);   // brown boat hull
    m_hullTex = device.createTexture(t.data(), 8, 8, true);
    return true;
}

void BoatDemo::setInput(const x3::phys::VehicleInput& in) { if (m_ctl) m_ctl->setInput(in); }
void BoatDemo::preStep(float dt)  { if (m_ctl) m_ctl->preStep(dt); }
void BoatDemo::postStep(float dt) { if (m_ctl) m_ctl->postStep(dt); }

void BoatDemo::hullPos(float out[3]) const {
    x3::phys::Vec3 p = m_physics ? m_physics->getBodyPosition(m_hull) : x3::phys::Vec3{};
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}

void BoatDemo::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device || !m_ctl) return;
    const float col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    x3::phys::Vec3 p = m_physics->getBodyPosition(m_hull);
    float q[4]; m_physics->getBodyRotation(m_hull, q);
    float pos[3] = { p.x, p.y, p.z };
    float m[16]; composeTRS(pos, q, m_hx*2.0f, m_hy*2.0f, m_hz*2.0f, m);
    m_device->drawMesh(frame, m_hullMesh, m_hullTex, col, m);
}

void BoatDemo::shutdown() {
    m_ctl.reset();
    if (m_physics && m_hull.valid()) m_physics->removeBody(m_hull);
    if (m_device) {
        if (m_hullMesh.valid()) m_device->destroyMesh(m_hullMesh);
        if (m_hullTex.valid())  m_device->destroyTexture(m_hullTex);
    }
    m_device = nullptr; m_physics = nullptr;
}

// ===========================================================================
// FlyDemo
// ===========================================================================
bool FlyDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                    float x, float y, float z) {
    m_device = &device; m_physics = &physics;
    m_body = physics.addBox(x3::phys::Vec3{m_hx, m_hy, m_hz},
                            x3::phys::Vec3{x, y, z}, 1200.0f, x3::phys::Layer::Dynamic);
    if (!m_body.valid()) return false;

    x3::phys::FlightDesc fd;
    fd.body = m_body;
    fd.maxThrust = 26000.0f; fd.liftCoefficient = 0.55f; fd.linearDrag = 0.5f;
    fd.pitchTorque = 16000.0f; fd.yawTorque = 8000.0f; fd.rollTorque = 16000.0f;
    fd.angularDamping = 2.0f; fd.gravity = true;
    m_ctl.reset(x3::phys::createFlightController(physics, fd));
    if (!m_ctl) { physics.removeBody(m_body); m_body = {}; return false; }

    std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
    x3::prims::makeCube(0.5f, cv, ci);
    m_bodyMesh = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
    m_wingMesh = m_bodyMesh; // reuse the cube for the wing (drawn flat + wide)
    auto t = x3::prims::makeSolidRGBA(8, 200, 200, 210);  // silver airframe
    m_bodyTex = device.createTexture(t.data(), 8, 8, true);
    return true;
}

void FlyDemo::setInput(const x3::phys::VehicleInput& in) { if (m_ctl) m_ctl->setInput(in); }
void FlyDemo::preStep(float dt)  { if (m_ctl) m_ctl->preStep(dt); }
void FlyDemo::postStep(float dt) { if (m_ctl) m_ctl->postStep(dt); }

void FlyDemo::airframePos(float out[3]) const {
    x3::phys::Vec3 p = m_physics ? m_physics->getBodyPosition(m_body) : x3::phys::Vec3{};
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}

void FlyDemo::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device || !m_ctl) return;
    const float fuselage[4] = { 0.85f, 0.85f, 0.9f, 1.0f };
    const float wingCol[4]  = { 0.65f, 0.65f, 0.72f, 1.0f };
    x3::phys::Vec3 p = m_physics->getBodyPosition(m_body);
    float q[4]; m_physics->getBodyRotation(m_body, q);
    float pos[3] = { p.x, p.y, p.z };
    // Fuselage (the body box).
    float m[16]; composeTRS(pos, q, m_hx*2.0f, m_hy*2.0f, m_hz*2.0f, m);
    m_device->drawMesh(frame, m_bodyMesh, m_bodyTex, fuselage, m);
    // Wing: a thin wide slab through the body (local: wide in X, thin in Y, short
    // in Z). Drawn at the same transform with a different scale (compose with the
    // chassis rotation: we re-use composeTRS with a wing-shaped scale).
    float wm[16]; composeTRS(pos, q, (m_hx*2.4f)*2.0f, (0.08f)*2.0f, (m_hz*0.5f)*2.0f, wm);
    m_device->drawMesh(frame, m_wingMesh, m_bodyTex, wingCol, wm);
}

void FlyDemo::shutdown() {
    m_ctl.reset();
    if (m_physics && m_body.valid()) m_physics->removeBody(m_body);
    if (m_device) {
        if (m_bodyMesh.valid()) m_device->destroyMesh(m_bodyMesh);
        if (m_bodyTex.valid())  m_device->destroyTexture(m_bodyTex);
    }
    m_device = nullptr; m_physics = nullptr;
}

} // namespace x3::game
