// Physics props (FEATURE_GOALS §1) — see app/physprops.h.
//
// Clean-room: built from the IPhysicsWorld + Scene interfaces only (dynamic
// bodies + the point-constraint joint API + per-body damping).
#include "physprops.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {
constexpr float kCubeHalf   = 0.35f;   // 0.7 m cube
constexpr float kCubeMass   = 5.0f;
constexpr float kHangLen    = 0.9f;    // pivot-to-cube-centre (pendulum length)
constexpr float kLinDamp    = 0.15f;
constexpr float kAngDamp    = 0.40f;
const   float   kPropTint[4]= { 0.80f, 0.55f, 0.20f, 1.0f };  // crate amber

// Column-major 4x4 from a quaternion (x,y,z,w) + translation (the same layout the
// door/monster render paths use). Mirrors JPH xyzw convention.
void composeTRS(float m[16], const float q[4], float tx, float ty, float tz) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x*x, yy = y*y, zz = z*z;
    const float xy = x*y, xz = x*z, yz = y*z;
    const float wx = w*x, wy = w*y, wz = w*z;
    m[0]  = 1 - 2*(yy+zz); m[1]  = 2*(xy+wz);     m[2]  = 2*(xz-wy);     m[3]  = 0;
    m[4]  = 2*(xy-wz);     m[5]  = 1 - 2*(xx+zz); m[6]  = 2*(yz+wx);     m[7]  = 0;
    m[8]  = 2*(xz+wy);     m[9]  = 2*(yz-wx);     m[10] = 1 - 2*(xx+yy); m[11] = 0;
    m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1;
}
} // namespace

void PhysPropsSystem::build(Scene& scene, x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics,
                            x3::phys::Vec3 origin, int count, float drop) {
    for (int i = 0; i < count; ++i) {
        const float cx = origin.x + ((float)i - (count - 1) * 0.5f) * 1.2f;  // 1.2 m apart
        const float cz = origin.z;
        const x3::phys::Vec3 anchor{ cx, origin.y + drop, cz };
        const x3::phys::Vec3 center{ cx, anchor.y - kHangLen, cz };          // hangs below

        // Render box authored centred at the body origin so the Entity transform
        // (driven from the body each frame) places + rotates it.
        x3::prims::PrimMesh geo = x3::prims::makeBox(kCubeHalf, kCubeHalf, kCubeHalf, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0]=kPropTint[0]; e.baseColor[1]=kPropTint[1];
        e.baseColor[2]=kPropTint[2]; e.baseColor[3]=kPropTint[3];
        e.tag = (uint32_t)Tag::Prop;
        e.body = physics.addBox(x3::phys::Vec3{kCubeHalf, kCubeHalf, kCubeHalf},
                                center, kCubeMass, x3::phys::Layer::Dynamic);
        e.transform[12]=center.x; e.transform[13]=center.y; e.transform[14]=center.z;
        uint32_t ent = scene.add(e);

        physics.setBodyDamping(e.body, kLinDamp, kAngDamp);
        // Pin the cube to the FIXED world anchor above (invalid `a` -> world).
        x3::phys::ConstraintId joint =
            physics.addPointConstraint(x3::phys::BodyId{}, e.body, anchor);

        m_props.push_back(PhysProp{ ent, e.body, joint });
    }
    x3::logInfo("PhysProps: built " + std::to_string(m_props.size()) +
                " hanging cube(s) on point joints");
}

void PhysPropsSystem::update(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    for (const PhysProp& p : m_props) {
        if (p.entity == kNoLink || p.entity >= scene.size()) continue;
        x3::phys::Vec3 pos = physics.getBodyPosition(p.body);
        float q[4]; physics.getBodyRotation(p.body, q);
        composeTRS(scene.get(p.entity).transform, q, pos.x, pos.y, pos.z);
    }
}

// ===========================================================================
// Headless self-test (--test-physprops). T0-T3.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[physprops-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[physprops-test] FAIL ") + name); }
}
constexpr float kDt = 1.0f / 60.0f;
}

bool runPhysPropsSelfTest() {
    g_pass = g_fail = 0;

    // ---- T0/T1: a cube pinned by a point joint HANGS (gravity held; no floor). --
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        x3::phys::BodyId box = w->addBox(x3::phys::Vec3{0.35f,0.35f,0.35f},
                                         x3::phys::Vec3{0,5,0}, 5.0f, x3::phys::Layer::Dynamic);
        w->setBodyDamping(box, 0.1f, 0.3f);
        x3::phys::ConstraintId j = w->addPointConstraint(x3::phys::BodyId{}, box,
                                                         x3::phys::Vec3{0,6,0});
        check(j.valid(), "T0 point joint created (cube pinned to world anchor)");
        for (int i = 0; i < 120; ++i) w->step(kDt);          // ~2 s
        x3::phys::Vec3 p = w->getBodyPosition(box);
        // Pendulum of length 1 hangs with its centre ~y=5 directly below the (0,6,0)
        // pivot. WITHOUT the joint, with no floor, it would fall far below.
        bool hung = p.y > 4.0f && std::abs(p.x) < 1.0f && std::abs(p.z) < 1.0f;
        check(hung, "T1 cube hangs from the point joint (did not fall)");
        w->shutdown();
    }

    // ---- T2: a sideways knock makes it SWING, then DAMPING bleeds the swing down.
    // A point joint lets the box rotate freely, so only LINEAR damping bleeds the
    // pendulum — use a firm value and compare the swing AMPLITUDE (peak |x|) in an
    // early window vs a late window (robust to which phase we sample at). --
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        x3::phys::BodyId box = w->addBox(x3::phys::Vec3{0.35f,0.35f,0.35f},
                                         x3::phys::Vec3{0,5,0}, 5.0f, x3::phys::Layer::Dynamic);
        w->setBodyDamping(box, 0.6f, 0.6f);
        w->addPointConstraint(x3::phys::BodyId{}, box, x3::phys::Vec3{0,6,0});
        const float shove[3] = { 5.0f, 0.0f, 0.0f };
        w->setBodyLinearVelocity(box, shove);                // knock it +X
        auto peakOver = [&](int frames) {
            float pk = 0.0f;
            for (int i = 0; i < frames; ++i) { w->step(kDt); pk = std::max(pk, std::fabs(w->getBodyPosition(box).x)); }
            return pk;
        };
        float earlyPeak = peakOver(180);                     // first ~3 s of swinging
        for (int i = 0; i < 300; ++i) w->step(kDt);          // let damping work ~5 s
        float latePeak  = peakOver(180);                     // next ~3 s of swinging
        bool swung    = earlyPeak > 0.2f;                    // it visibly swung out
        bool settling = latePeak  < earlyPeak * 0.5f;        // amplitude decayed (settling)
        check(swung && settling, "T2 cube swings on a knock then damps down (settles)");
        w->shutdown();
    }

    // ---- T3: removing the joint drops the cube (the joint was load-bearing). -----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        x3::phys::BodyId box = w->addBox(x3::phys::Vec3{0.35f,0.35f,0.35f},
                                         x3::phys::Vec3{0,5,0}, 5.0f, x3::phys::Layer::Dynamic);
        x3::phys::ConstraintId j = w->addPointConstraint(x3::phys::BodyId{}, box,
                                                         x3::phys::Vec3{0,6,0});
        for (int i = 0; i < 60; ++i) w->step(kDt);
        float yBefore = w->getBodyPosition(box).y;
        w->removeConstraint(j);
        for (int i = 0; i < 180; ++i) w->step(kDt);          // ~3 s free fall
        float yAfter = w->getBodyPosition(box).y;
        check(yAfter < yBefore - 2.0f, "T3 removing the joint drops the cube (free fall)");
        w->shutdown();
    }

    x3::logInfo(std::string("[physprops-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
