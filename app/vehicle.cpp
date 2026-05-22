// Vehicle demo worlds — implementation. See vehicle.h.
//
// Built only through the public engine interfaces (IRenderDevice / IPhysicsWorld /
// IVehicleController). Clean-room.

#include "vehicle.h"

#include <cmath>
#include <cstring>

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
bool DriveDemo::build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                      float x, float y, float z) {
    m_device = &device; m_physics = &physics;

    // --- Chassis dynamic body (a box). Layer Dynamic. ---
    m_chassis = physics.addBox(x3::phys::Vec3{m_hx, m_hy, m_hz},
                               x3::phys::Vec3{x, y, z}, 1500.0f, x3::phys::Layer::Dynamic);
    if (!m_chassis.valid()) return false;

    // --- 4 wheels: front (-Z) steered, rear (+Z) powered + handbrake. ---
    m_wheels.clear();
    struct P { float wx, wz; bool steer, hb; bool powered; };
    P p[4] = {
        { -m_hx, -m_hz, true,  false, false },   // front-left
        {  m_hx, -m_hz, true,  false, false },   // front-right
        { -m_hx,  m_hz, false, true,  true  },   // rear-left  (drive)
        {  m_hx,  m_hz, false, true,  true  },   // rear-right (drive)
    };
    for (int i = 0; i < 4; ++i) {
        x3::phys::WheelDesc w;
        // Attach at the chassis BOTTOM so the springs hold the belly above ground.
        w.position[0] = p[i].wx; w.position[1] = -m_hy; w.position[2] = p[i].wz;
        w.radius = 0.4f; w.width = 0.3f;
        w.suspensionMin = 0.10f; w.suspensionMax = 0.35f;
        w.suspensionFreq = 2.0f; w.suspensionDamp = 0.6f;
        w.steered = p[i].steer; w.handBraked = p[i].hb; w.powered = p[i].powered;
        w.maxSteerAngle = 0.5236f; // ~30deg
        m_wheels.push_back(w);
    }
    x3::phys::WheeledVehicleDesc vd;
    vd.chassis = m_chassis;
    vd.wheels = m_wheels.data(); vd.wheelCount = (uint32_t)m_wheels.size();
    vd.maxEngineTorque = 700.0f; vd.maxEngineRPM = 6000.0f;
    // Wheel rays cast as Dynamic so they stand on the Static terrain (Static-vs-
    // Static doesn't collide in the engine matrix; Dynamic-vs-Static does).
    vd.groundLayer = x3::phys::Layer::Dynamic;
    m_ctl.reset(x3::phys::createWheeledVehicle(physics, vd));
    if (!m_ctl) { physics.removeBody(m_chassis); m_chassis = {}; return false; }

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

void DriveDemo::setInput(const x3::phys::VehicleInput& in) { if (m_ctl) m_ctl->setInput(in); }
void DriveDemo::preStep(float dt)  { if (m_ctl) m_ctl->preStep(dt); }
void DriveDemo::postStep(float dt) { if (m_ctl) m_ctl->postStep(dt); }

void DriveDemo::chassisPos(float out[3]) const {
    x3::phys::Vec3 p = m_physics ? m_physics->getBodyPosition(m_chassis) : x3::phys::Vec3{};
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}

void DriveDemo::render(const x3::rhi::FrameContext& frame) const {
    if (!m_device || !m_ctl) return;
    const float bodyCol[4]  = { 1.0f, 0.25f, 0.22f, 1.0f };
    const float wheelCol[4] = { 0.12f, 0.12f, 0.14f, 1.0f };

    // Chassis: pos + rotation from physics, scaled to its half extents (cube is
    // half-extent 0.5 -> scale = he/0.5 = he*2).
    x3::phys::Vec3 p = m_physics->getBodyPosition(m_chassis);
    float q[4]; m_physics->getBodyRotation(m_chassis, q);
    float pos[3] = { p.x, p.y, p.z };
    float m[16]; composeTRS(pos, q, m_hx*2.0f, m_hy*2.0f, m_hz*2.0f, m);
    m_device->drawMesh(frame, m_chassisMesh, m_chassisTex, bodyCol, m);

    // Wheels: the controller hands back a world transform that already maps a
    // unit Y-cylinder (radius 1, half-height 1) to the wheel. Our cylinder mesh is
    // radius 1, height 1 (half-height 0.5), so the controller's transform (which
    // bakes radius into x/z and half-width into y) places it correctly.
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
    m_device = nullptr; m_physics = nullptr;
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
