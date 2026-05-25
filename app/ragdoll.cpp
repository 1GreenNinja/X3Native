// Ragdoll (FEATURE_GOALS §2) — see app/ragdoll.h.
//
// Clean-room: built from the IPhysicsWorld dynamic-body + point-constraint API +
// Scene only. The same primitives the physics props (§1) use.
#include "ragdoll.h"
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstring>
#include <functional>
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

// Inverse of a RIGID column-major 4x4 (rotation R + translation t, no scale):
// inv = [ R^T | -R^T t ]. Cheap + exact for body poses.
void invRigid(const float m[16], float out[16]) {
    // Column-major: basis columns are m[0..2], m[4..6], m[8..10]; translation m[12..14].
    out[0]=m[0]; out[1]=m[4]; out[2]=m[8];  out[3]=0;
    out[4]=m[1]; out[5]=m[5]; out[6]=m[9];  out[7]=0;
    out[8]=m[2]; out[9]=m[6]; out[10]=m[10];out[11]=0;
    const float tx=m[12], ty=m[13], tz=m[14];
    out[12] = -(out[0]*tx + out[4]*ty + out[8]*tz);
    out[13] = -(out[1]*tx + out[5]*ty + out[9]*tz);
    out[14] = -(out[2]*tx + out[6]*ty + out[10]*tz);
    out[15] = 1.0f;
}
} // namespace

// ---------------------------------------------------------------------------
// RagdollSkin — rigid-attach bone driver.
// ---------------------------------------------------------------------------
bool RagdollSkin::bind(const x3::asset::Model& model) {
    m_nodeCount = (uint32_t)model.nodes.size();
    m_assign.clear();
    if (m_nodeCount == 0) return false;
    m_bindGlobal.assign((size_t)m_nodeCount * 16, 0.0f);

    // Compose each node's bind-pose global (parent-first, memoized; nodes may be
    // in any order). global[n] = global[parent] * local[n] (column-major).
    std::vector<char> done(m_nodeCount, 0);
    std::function<void(int)> resolve = [&](int n) {
        if (n < 0 || (uint32_t)n >= m_nodeCount || done[n]) return;
        const x3::asset::Node& nd = model.nodes[n];
        if (nd.parent < 0) {
            std::memcpy(&m_bindGlobal[(size_t)n*16], nd.localTransform, 16*sizeof(float));
        } else {
            resolve(nd.parent);
            x3::asset::mulMat4(&m_bindGlobal[(size_t)nd.parent*16], nd.localTransform,
                               &m_bindGlobal[(size_t)n*16]);
        }
        done[n] = 1;
    };
    for (uint32_t n = 0; n < m_nodeCount; ++n) resolve((int)n);
    return true;
}

bool RagdollSkin::bindFromGlobals(const x3::asset::Model& model,
                                  const float* nodeGlobals, uint32_t nodeCount) {
    m_nodeCount = (uint32_t)model.nodes.size();
    m_assign.clear();
    if (m_nodeCount == 0 || !nodeGlobals || nodeCount != m_nodeCount) {
        // Fall back to the static bind pose so we never leave the skin unbound.
        return bind(model);
    }
    // Seed the reference globals directly from the supplied (current animated) pose.
    m_bindGlobal.assign((size_t)m_nodeCount * 16, 0.0f);
    std::memcpy(m_bindGlobal.data(), nodeGlobals, (size_t)m_nodeCount * 16 * sizeof(float));
    return true;
}

void RagdollSkin::mapToParts(const float* partInit, uint32_t partCount) {
    if (m_nodeCount == 0 || !partInit || partCount == 0) return;
    m_partCount = partCount;
    m_partInitInv.assign((size_t)partCount * 16, 0.0f);
    for (uint32_t p = 0; p < partCount; ++p)
        invRigid(&partInit[(size_t)p*16], &m_partInitInv[(size_t)p*16]);

    // Assign each node to the nearest part center (bind-pose positions).
    m_assign.assign(m_nodeCount, 0);
    for (uint32_t n = 0; n < m_nodeCount; ++n) {
        const float nx = m_bindGlobal[(size_t)n*16+12];
        const float ny = m_bindGlobal[(size_t)n*16+13];
        const float nz = m_bindGlobal[(size_t)n*16+14];
        float best = 1e30f; int bp = 0;
        for (uint32_t p = 0; p < partCount; ++p) {
            const float px = partInit[(size_t)p*16+12];
            const float py = partInit[(size_t)p*16+13];
            const float pz = partInit[(size_t)p*16+14];
            const float d = (nx-px)*(nx-px) + (ny-py)*(ny-py) + (nz-pz)*(nz-pz);
            if (d < best) { best = d; bp = (int)p; }
        }
        m_assign[n] = bp;
    }
}

uint32_t RagdollSkin::computeNodeGlobals(const float* partCur, uint32_t partCount,
                                         std::vector<float>& outNodeGlobals) const {
    if (m_nodeCount == 0 || m_assign.empty() || !partCur || partCount != m_partCount) {
        outNodeGlobals.clear(); return 0;
    }
    // Per-part delta = cur * initInv (rigid motion since bind).
    std::vector<float> delta((size_t)partCount * 16);
    for (uint32_t p = 0; p < partCount; ++p)
        x3::asset::mulMat4(&partCur[(size_t)p*16], &m_partInitInv[(size_t)p*16],
                           &delta[(size_t)p*16]);
    outNodeGlobals.assign((size_t)m_nodeCount * 16, 0.0f);
    for (uint32_t n = 0; n < m_nodeCount; ++n)
        x3::asset::mulMat4(&delta[(size_t)m_assign[n]*16], &m_bindGlobal[(size_t)n*16],
                           &outNodeGlobals[(size_t)n*16]);
    return m_nodeCount;
}

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

// ===========================================================================
// Headless self-test (--test-ragdollskin). S0-S3 — the rigid-attach math.
// ===========================================================================
bool runRagdollSkinSelfTest() {
    g_pass = g_fail = 0;

    // Synthetic model: a 3-node vertical chain (root at y=0, mid y=1, top y=2),
    // each a child of the previous (local translate +1 in Y). No skin needed for
    // the math test — we exercise bind globals + rigid attach directly.
    x3::asset::Model model;
    auto idLocalY = [](float y, x3::asset::Node& n, int parent) {
        for (int i = 0; i < 16; ++i) n.localTransform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        n.localTransform[13] = y;   // local +Y offset from parent
        n.parent = parent;
    };
    model.nodes.resize(3);
    idLocalY(0.0f, model.nodes[0], -1);
    idLocalY(1.0f, model.nodes[1], 0);
    idLocalY(1.0f, model.nodes[2], 1);

    RagdollSkin skin;
    bool bound = skin.bind(model);
    // Bind globals should put node 0 at y=0, node 1 at y=1, node 2 at y=2.
    std::vector<float> ident;  // 2 parts identity at the bind centers of node0 & node2.
    check(bound && skin.nodeCount() == 3, "S0 RagdollSkin bound a 3-node chain");

    // Two parts: part 0 at (0,0,0) [near node 0/1], part 1 at (0,2,0) [near node 2].
    float partInit[32];
    for (int i = 0; i < 32; ++i) partInit[i] = (i % 5 == 0) ? 1.0f : 0.0f; // two identities
    partInit[13] = 0.0f;   // part 0 translation y=0
    partInit[16+13] = 2.0f; // part 1 translation y=2
    skin.mapToParts(partInit, 2);

    // S1: parts AT their bind transform -> node globals reproduce the bind globals.
    std::vector<float> g0;
    skin.computeNodeGlobals(partInit, 2, g0);
    bool reproduced = g0.size() == 48 &&
                      std::fabs(g0[0*16+13] - 0.0f) < 1e-4f &&
                      std::fabs(g0[1*16+13] - 1.0f) < 1e-4f &&
                      std::fabs(g0[2*16+13] - 2.0f) < 1e-4f;
    check(reproduced, "S1 parts at bind reproduce the bind globals (no drift)");

    // S2: translate part 1 by +3 in X -> node 2 (assigned to part 1) moves +3 in X,
    // node 0 (assigned to part 0) does NOT move.
    float partCur[32]; std::memcpy(partCur, partInit, sizeof(partCur));
    partCur[16+12] += 3.0f;   // part 1 translation x += 3
    std::vector<float> g1;
    skin.computeNodeGlobals(partCur, 2, g1);
    bool node2Moved = std::fabs(g1[2*16+12] - 3.0f) < 1e-4f;     // node2.x == 3
    bool node0Still = std::fabs(g1[0*16+12] - 0.0f) < 1e-4f;     // node0.x == 0
    check(node2Moved && node0Still, "S2 moving a part rigidly moves only its assigned nodes");

    // S3: a part TRANSLATION carries the assigned node's Y too (rigid, not just X).
    float partCur2[32]; std::memcpy(partCur2, partInit, sizeof(partCur2));
    partCur2[16+13] += 0.5f;  // part 1 up 0.5
    std::vector<float> g2;
    skin.computeNodeGlobals(partCur2, 2, g2);
    bool node2Up = std::fabs(g2[2*16+13] - 2.5f) < 1e-4f;       // node2.y 2 -> 2.5
    check(node2Up, "S3 part translation carries its node rigidly (Y)");

    x3::logInfo(std::string("[ragdollskin-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
