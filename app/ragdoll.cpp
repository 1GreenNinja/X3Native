// Ragdoll (FEATURE_GOALS §2) — see app/ragdoll.h.
//
// Clean-room: built from the IPhysicsWorld dynamic-body + point-constraint API +
// Scene only. The same primitives the physics props (§1) use.
#include "ragdoll.h"
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {
// ---- Humanoid part layout (LOCAL, feet at y=0, facing +Z, ~1.8 m at scale 1).
// Each part: half-extents (hx,hy,hz) + local center (cx,cy,cz). Joints connect
// adjacent parts at the anchor between them.
struct PartDef { float hx, hy, hz, cx, cy, cz; float mass; };
enum { PELVIS = 0, TORSO, HEAD, ARM_L, ARM_R, LEG_L, LEG_R, NUM_PARTS };

const PartDef kParts[NUM_PARTS] = {
    /*PELVIS*/ { 0.18f, 0.13f, 0.12f,  0.00f, 0.95f, 0.0f, 12.0f },
    /*TORSO */ { 0.20f, 0.22f, 0.13f,  0.00f, 1.32f, 0.0f, 22.0f },
    /*HEAD  */ { 0.12f, 0.14f, 0.12f,  0.00f, 1.70f, 0.0f,  5.0f },
    /*ARM_L */ { 0.07f, 0.21f, 0.07f, -0.28f, 1.34f, 0.0f,  6.0f },
    /*ARM_R */ { 0.07f, 0.21f, 0.07f,  0.28f, 1.34f, 0.0f,  6.0f },
    /*LEG_L */ { 0.09f, 0.34f, 0.10f, -0.10f, 0.50f, 0.0f, 11.0f },
    /*LEG_R */ { 0.09f, 0.34f, 0.10f,  0.10f, 0.50f, 0.0f, 11.0f },
};
// Joints: (parent, child, local anchor xyz between them).
struct JointDef { int a, b; float ax, ay, az; };
const JointDef kJoints[] = {
    { PELVIS, TORSO,  0.00f, 1.13f, 0.0f },   // spine
    { TORSO,  HEAD,   0.00f, 1.55f, 0.0f },   // neck
    { TORSO,  ARM_L, -0.22f, 1.52f, 0.0f },   // L shoulder
    { TORSO,  ARM_R,  0.22f, 1.52f, 0.0f },   // R shoulder
    { PELVIS, LEG_L, -0.10f, 0.83f, 0.0f },   // L hip
    { PELVIS, LEG_R,  0.10f, 0.83f, 0.0f },   // R hip
};
constexpr int NUM_JOINTS = (int)(sizeof(kJoints) / sizeof(kJoints[0]));

constexpr float kLinDamp = 0.25f;
constexpr float kAngDamp = 0.45f;

// Column-major 4x4 from quaternion (x,y,z,w) + translation (matches the door /
// monster / physprops render convention; JPH xyzw).
void composeTRS(float m[16], const float q[4], float tx, float ty, float tz) {
    const float x=q[0], y=q[1], z=q[2], w=q[3];
    const float xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z, wx=w*x, wy=w*y, wz=w*z;
    m[0]=1-2*(yy+zz); m[1]=2*(xy+wz);   m[2]=2*(xz-wy);   m[3]=0;
    m[4]=2*(xy-wz);   m[5]=1-2*(xx+zz); m[6]=2*(yz+wx);   m[7]=0;
    m[8]=2*(xz+wy);   m[9]=2*(yz-wx);   m[10]=1-2*(xx+yy);m[11]=0;
    m[12]=tx; m[13]=ty; m[14]=tz; m[15]=1;
}

// Rotate a local (x,z) about +Y by yaw, into world (the ragdoll's facing).
void yawXZ(float yaw, float lx, float lz, float& ox, float& oz) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    ox = c * lx + s * lz;
    oz = -s * lx + c * lz;
}
} // namespace

void RagdollSystem::build(Scene& scene, x3::rhi::IRenderDevice& device,
                          x3::phys::IPhysicsWorld& physics,
                          x3::phys::Vec3 footPos, float yaw, float scale,
                          const float tint[4], x3::phys::Vec3 impulse) {
    clear(physics);
    if (scale <= 0.0f) scale = 1.0f;

    // World center of each part = footPos + yaw-rotated, scaled local center.
    auto worldCenter = [&](const PartDef& p) {
        float ox, oz; yawXZ(yaw, p.cx * scale, p.cz * scale, ox, oz);
        return x3::phys::Vec3{ footPos.x + ox, footPos.y + p.cy * scale, footPos.z + oz };
    };

    m_parts.reserve(NUM_PARTS);
    for (int i = 0; i < NUM_PARTS; ++i) {
        const PartDef& p = kParts[i];
        const x3::phys::Vec3 c = worldCenter(p);
        const x3::phys::Vec3 half{ p.hx * scale, p.hy * scale, p.hz * scale };

        x3::prims::PrimMesh geo = x3::prims::makeBox(half.x, half.y, half.z, 0, 0, 0, 1.0f);
        Entity e;
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
        e.baseColor[0]=tint[0]; e.baseColor[1]=tint[1]; e.baseColor[2]=tint[2]; e.baseColor[3]=tint[3];
        e.tag  = (uint32_t)Tag::Prop;
        e.body = physics.addBox(half, c, p.mass, x3::phys::Layer::Dynamic);
        e.transform[12]=c.x; e.transform[13]=c.y; e.transform[14]=c.z;
        uint32_t ent = scene.add(e);
        physics.setBodyDamping(e.body, kLinDamp, kAngDamp);
        m_parts.push_back(RagdollPart{ ent, e.body, half.y });
    }

    // Link adjacent parts with point joints at the world anchor between them.
    for (int j = 0; j < NUM_JOINTS; ++j) {
        const JointDef& jd = kJoints[j];
        float ox, oz; yawXZ(yaw, jd.ax * scale, jd.az * scale, ox, oz);
        const x3::phys::Vec3 anchor{ footPos.x + ox, footPos.y + jd.ay * scale, footPos.z + oz };
        x3::phys::ConstraintId c =
            physics.addPointConstraint(m_parts[jd.a].body, m_parts[jd.b].body, anchor);
        if (c.valid()) m_joints.push_back(c);
    }

    // Death kick. A ball-jointed box stack can PROP ITSELF upright via part-on-part
    // contact, so a gentle shove just makes it wobble and re-settle (not a death).
    // To read as a real collapse we (a) shove the torso and (b) drive a decisive
    // TOPPLE: rotate the torso + pelvis about a horizontal axis so the centre of
    // mass swings past the feet and the whole body goes down. The topple axis is
    // perpendicular to the shove direction (it falls the way it was hit); with no
    // shove it falls sideways. Magnitude is large on purpose — limp bodies drop.
    if (impulse.x != 0.0f || impulse.y != 0.0f || impulse.z != 0.0f)
        physics.applyImpulse(m_parts[TORSO].body, impulse);
    // Horizontal shove dir (XZ); default sideways (+X) if no impulse given.
    float hx = impulse.x, hz = impulse.z;
    float hl = std::sqrt(hx*hx + hz*hz);
    if (hl < 1e-3f) { hx = 1.0f; hz = 0.0f; hl = 1.0f; }
    hx /= hl; hz /= hl;
    // Topple about the axis perpendicular to the shove (cross with +Y): (hz,0,-hx).
    const float w = 6.0f;   // rad/s — tips well past the balance point
    const float topple[3] = { hz * w, 0.0f, -hx * w };
    physics.setBodyAngularVelocity(m_parts[TORSO].body,  topple);
    physics.setBodyAngularVelocity(m_parts[PELVIS].body, topple);

    x3::logInfo("Ragdoll: built " + std::to_string(m_parts.size()) + " parts, " +
                std::to_string(m_joints.size()) + " joints");
}

void RagdollSystem::update(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    for (const RagdollPart& p : m_parts) {
        if (p.entity == kNoLink || p.entity >= scene.size()) continue;
        x3::phys::Vec3 pos = physics.getBodyPosition(p.body);
        float q[4]; physics.getBodyRotation(p.body, q);
        composeTRS(scene.get(p.entity).transform, q, pos.x, pos.y, pos.z);
    }
}

void RagdollSystem::clear(x3::phys::IPhysicsWorld& physics) {
    for (x3::phys::ConstraintId c : m_joints) physics.removeConstraint(c);
    m_joints.clear();
    for (const RagdollPart& p : m_parts)
        if (p.body.valid()) physics.removeBody(p.body);
    m_parts.clear();
}

// ===========================================================================
// Headless self-test (--test-ragdoll). T0-T3.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[ragdoll-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[ragdoll-test] FAIL ") + name); }
}
constexpr float kDt = 1.0f / 60.0f;

// Headless device: the shared no-op test double (mints valid mesh handles).
using HeadlessDevice = x3::game::HeadlessRenderDevice;

float dist(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx=a.x-b.x, dy=a.y-b.y, dz=a.z-b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}
}

bool runRagdollSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
    w->init();
    HeadlessDevice device;
    Scene scene;

    // A big static floor at y=0 to catch the collapse.
    w->addBox(x3::phys::Vec3{ 20.0f, 0.5f, 20.0f }, x3::phys::Vec3{ 0, -0.5f, 0 },
              0.0f, x3::phys::Layer::Static);

    RagdollSystem rag;
    const float tint[4] = { 0.7f, 0.3f, 0.3f, 1.0f };
    rag.build(scene, device, *w, x3::phys::Vec3{ 0, 0, 0 }, 0.0f, 1.0f, tint,
              x3::phys::Vec3{ 30.0f, 10.0f, 0.0f });   // kicked +X / up

    // ---- T0: 7 parts built. ----
    check(rag.partCount() == 7, "T0 ragdoll built with 7 parts");

    // Record initial head/torso heights + their separation.
    auto bodyPos = [&](int part){ return w->getBodyPosition(rag.part(part).body); };
    const float headY0 = bodyPos(2).y;                  // HEAD index 2
    const float sep0 = dist(bodyPos(1), bodyPos(2));    // torso<->head rest distance

    // Step ~5 s for the collapse + settle.
    for (int i = 0; i < 300; ++i) {
        w->step(kDt);
        rag.update(scene, *w);
    }

    // ---- T1: it COLLAPSED — the head dropped toward the floor (was ~1.7 m). ----
    const float headY1 = bodyPos(2).y;
    check(headY1 < headY0 - 0.6f && headY1 < 0.9f, "T1 ragdoll collapsed to the floor");

    // ---- T2: joints HELD — head stayed near the torso (didn't explode apart). ----
    const float sep1 = dist(bodyPos(1), bodyPos(2));
    check(sep1 < sep0 + 0.35f, "T2 joints held the body together (no explosion)");

    // ---- T3: it came to REST (all parts nearly stopped). ----
    float maxSpeed = 0.0f;
    for (uint32_t i = 0; i < rag.partCount(); ++i) {
        float v[3]; w->getBodyLinearVelocity(rag.part(i).body, v);
        maxSpeed = std::max(maxSpeed, std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]));
    }
    check(maxSpeed < 0.6f, "T3 ragdoll settled to rest");

    // ---- cleanup is well-behaved. ----
    rag.clear(*w);
    check(rag.partCount() == 0, "T4 clear() removes all parts");

    w->shutdown();
    x3::logInfo(std::string("[ragdoll-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
