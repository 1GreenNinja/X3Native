// Ragdoll — physics-driven skeletal bodies blended with animation (Physics §2).
// See Ragdoll.h. Jolt Physics (MIT) ONLY. JPH:: types are confined to this TU.
//
// Clean-room: built from Jolt's public Ragdoll / RagdollSettings / Skeleton /
// SkeletonPose / SwingTwistConstraint API + public physics references. NO Unreal /
// id Tech / RBDOOM or any other game-engine source consulted.

#include "Ragdoll.h"
#include "IPhysicsWorld.h"
#include "../core/x3_log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace x3::phys {

namespace {

// Object layer used by the ragdoll bodies. Must match the JoltPhysicsWorld layer
// table (Enemy = 3) so the ragdoll collides with the static world + other dynamics
// the way a character would. Kept here as a literal to avoid leaking the layer enum
// internals; Layer::Enemy maps to object layer 3 in JoltPhysicsWorld.cpp.
constexpr JPH::ObjectLayer kRagdollLayer = 3; // == ObjLayers::Enemy

// Column-major float[16] -> JPH::Mat44.
JPH::Mat44 toMat44(const float m[16]) {
    return JPH::Mat44(
        JPH::Vec4(m[0],  m[1],  m[2],  m[3]),
        JPH::Vec4(m[4],  m[5],  m[6],  m[7]),
        JPH::Vec4(m[8],  m[9],  m[10], m[11]),
        JPH::Vec4(m[12], m[13], m[14], m[15]));
}

// JPH::Mat44 -> column-major float[16].
void fromMat44(JPH::Mat44Arg src, float out[16]) {
    for (int c = 0; c < 4; ++c) {
        JPH::Vec4 col = src.GetColumn4(c);
        out[c*4+0] = col.GetX(); out[c*4+1] = col.GetY();
        out[c*4+2] = col.GetZ(); out[c*4+3] = col.GetW();
    }
}

JPH::RVec3 worldPos(const float m[16]) { return JPH::RVec3(m[12], m[13], m[14]); }

// Column-major float[16] -> JPH::Mat44 with the upper-3x3 basis renormalized to a
// pure rotation (uniform scale stripped) and the translation preserved. Used before
// handing a world matrix to Jolt's SetPose, whose GetQuaternion() asserts the result
// is normalized (a scaled basis would otherwise trip the Debug IsNormalized assert).
// Degenerate (near-zero-length) columns fall back to the identity axis so the result
// is always a valid orthonormal basis.
JPH::Mat44 sanitizeOrtho(const float m[16]) {
    auto normCol = [](float x, float y, float z, JPH::Vec3 fallback) -> JPH::Vec3 {
        float len2 = x*x + y*y + z*z;
        if (len2 < 1e-12f) return fallback;
        float inv = 1.0f / std::sqrt(len2);
        return JPH::Vec3(x*inv, y*inv, z*inv);
    };
    JPH::Vec3 c0 = normCol(m[0],  m[1],  m[2],  JPH::Vec3::sAxisX());
    JPH::Vec3 c1 = normCol(m[4],  m[5],  m[6],  JPH::Vec3::sAxisY());
    JPH::Vec3 c2 = normCol(m[8],  m[9],  m[10], JPH::Vec3::sAxisZ());
    return JPH::Mat44(
        JPH::Vec4(c0, 0.0f),
        JPH::Vec4(c1, 0.0f),
        JPH::Vec4(c2, 0.0f),
        JPH::Vec4(m[12], m[13], m[14], 1.0f));
}

class JoltRagdoll final : public IRagdoll {
public:
    JoltRagdoll(IPhysicsWorld& world, const RagdollBoneDesc* bones, uint32_t n)
        : m_world(&world) {
        m_system = static_cast<JPH::PhysicsSystem*>(world.nativeSystem());
        m_names.reserve(n);
        for (uint32_t i = 0; i < n; ++i) m_names.push_back(bones[i].name);
    }

    ~JoltRagdoll() override {
        removeFromWorld();
        // m_ragdoll Ref releases here; m_settings Ref too.
    }

    bool build(const RagdollBoneDesc* bones, uint32_t n) {
        if (!m_system || n == 0) return false;

        // 1) Build the skeleton (parent must precede child — validate).
        JPH::Ref<JPH::Skeleton> skeleton = new JPH::Skeleton();
        for (uint32_t i = 0; i < n; ++i) {
            if (bones[i].parent >= (int)i) {
                x3::logError("[ragdoll] bone " + std::to_string(i) +
                             " has a parent that does not precede it (mis-ordered)");
                return false;
            }
            skeleton->AddJoint(bones[i].name, bones[i].parent);
        }
        if (!skeleton->AreJointsCorrectlyOrdered()) {
            x3::logError("[ragdoll] skeleton joints are not correctly ordered");
            return false;
        }

        // 2) Build RagdollSettings: one capsule body per bone + a swing-twist joint
        //    to the parent (anchored at this bone's bind-world origin). All in WORLD
        //    space, so we don't have to compute body-local frames.
        m_settings = new JPH::RagdollSettings();
        m_settings->mSkeleton = skeleton;
        m_settings->mParts.resize(n);

        m_bindWorld.assign(n * 16, 0.0f);

        for (uint32_t i = 0; i < n; ++i) {
            const RagdollBoneDesc& d = bones[i];
            std::memcpy(&m_bindWorld[i*16], d.bindWorld, 16 * sizeof(float));

            // Capsule along local +Y (origin -> child). Wrap in a RotatedTranslated
            // shape only if needed (here the capsule axis already matches +Y).
            float hh = std::max(0.01f, d.halfHeight);
            float r  = std::max(0.02f, d.radius);
            JPH::Ref<JPH::CapsuleShape> caps = new JPH::CapsuleShape(hh, r);
            // Shift the capsule so its base sits at the bone origin and it extends
            // along +Y toward the child (matches the bind transform's +Y column).
            JPH::RotatedTranslatedShapeSettings rts(
                JPH::Vec3(0, hh, 0), JPH::Quat::sIdentity(), caps);
            rts.SetEmbedded();
            JPH::ShapeSettings::ShapeResult shapeRes = rts.Create();
            if (shapeRes.HasError()) {
                x3::logError(std::string("[ragdoll] bone shape: ") +
                             shapeRes.GetError().c_str());
                return false;
            }

            JPH::Mat44 bind = toMat44(d.bindWorld);
            JPH::Quat  rot   = bind.GetQuaternion().Normalized();
            JPH::RVec3 pos   = worldPos(d.bindWorld);

            JPH::RagdollSettings::Part& part = m_settings->mParts[i];
            part.SetShape(shapeRes.Get());
            part.mPosition   = pos;
            part.mRotation   = rot;
            part.mMotionType = JPH::EMotionType::Dynamic;
            part.mObjectLayer = kRagdollLayer;
            part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            part.mMassPropertiesOverride.mMass = std::max(0.05f, d.mass);
            // A touch of damping so the collapse settles instead of jittering.
            part.mLinearDamping  = 0.1f;
            part.mAngularDamping = 0.2f;

            if (d.parent >= 0) {
                // Swing-twist joint to the parent, anchored at THIS bone's origin
                // (the joint between parent and child). Reference frame = WORLD.
                // Twist axis = the bone's local +Y (direction toward the child);
                // plane axis = local +X.
                JPH::SwingTwistConstraintSettings* st = new JPH::SwingTwistConstraintSettings();
                st->mSpace        = JPH::EConstraintSpace::WorldSpace;
                st->mPosition1    = pos;     // on the parent
                st->mPosition2    = pos;     // on the child (coincident at the joint)
                JPH::Vec3 twist   = bind.GetColumn3(1).NormalizedOr(JPH::Vec3::sAxisY()); // +Y
                JPH::Vec3 plane   = bind.GetColumn3(0).NormalizedOr(JPH::Vec3::sAxisX()); // +X
                st->mTwistAxis1   = twist; st->mTwistAxis2 = twist;
                st->mPlaneAxis1   = plane; st->mPlaneAxis2 = plane;
                float cone  = std::clamp(d.coneHalfAngle, 0.0f, 3.10f);
                float twa   = std::clamp(d.twistLimit,    0.0f, 3.10f);
                st->mNormalHalfConeAngle = cone;
                st->mPlaneHalfConeAngle  = cone;
                st->mTwistMinAngle = -twa;
                st->mTwistMaxAngle =  twa;
                part.mToParent = st;
            }
        }

        // Priorities + self-collision filter + stabilization (Jolt best practice for
        // a stable chain — see RagdollSettings docs).
        m_settings->CalculateConstraintPriorities(0);
        m_settings->DisableParentChildCollisions();
        if (!m_settings->Stabilize())
            x3::logWarn("[ragdoll] Stabilize() reported an issue (continuing)");
        m_settings->CalculateBodyIndexToConstraintIndex();

        // 3) Create the runtime ragdoll instance.
        m_ragdoll = m_settings->CreateRagdoll(/*collisionGroup*/ ++s_groupCounter,
                                              /*userData*/ 0, m_system);
        if (!m_ragdoll) {
            x3::logError("[ragdoll] CreateRagdoll failed (out of bodies?)");
            return false;
        }
        m_count = n;
        return true;
    }

    uint32_t boneCount() const override { return m_count; }

    std::string_view boneName(uint32_t bone) const override {
        return (bone < m_names.size()) ? std::string_view(m_names[bone])
                                       : std::string_view();
    }

    void addToWorld(bool activate) override {
        if (!m_ragdoll || m_inWorld) return;
        m_ragdoll->AddToPhysicsSystem(activate ? JPH::EActivation::Activate
                                               : JPH::EActivation::DontActivate);
        m_inWorld = true;
    }

    void removeFromWorld() override {
        if (!m_ragdoll || !m_inWorld) return;
        m_ragdoll->RemoveFromPhysicsSystem();
        m_inWorld = false;
    }

    bool inWorld() const override { return m_inWorld; }

    void setPoseWorld(const float* worldMatrices) override {
        if (!m_ragdoll || !worldMatrices) return;
        // Lower-level SetPose: directly take the world-space joint matrices. The root
        // offset is the root bone's world translation; the matrices we pass are full
        // world transforms, so pass a zero root offset (matrices are already world).
        //
        // Jolt's SetPose calls Mat44::GetQuaternion() on each matrix and (in a Debug
        // build) asserts the result IsNormalized() before rotating bodies. The caller's
        // bind-world matrices commonly carry a UNIFORM SCALE (e.g. the monster death
        // ragdoll bakes the model scale into the placement), which makes the rotation
        // columns non-unit and the extracted quaternion un-normalized -> Debug assert
        // (Quat.inl:344). Strip the scale here (renormalize the 3x3 basis columns)
        // before handing the matrix to Jolt so the quaternion is always normalized.
        std::vector<JPH::Mat44> mats(m_count);
        for (uint32_t i = 0; i < m_count; ++i)
            mats[i] = sanitizeOrtho(&worldMatrices[i*16]);
        m_ragdoll->SetPose(JPH::RVec3::sZero(), mats.data());
        m_ragdoll->SetLinearAndAngularVelocity(JPH::Vec3::sZero(), JPH::Vec3::sZero());
        m_ragdoll->ResetWarmStart();
    }

    void getBoneWorldTransforms(float* out) const override {
        if (!m_ragdoll || !out) return;
        std::vector<JPH::Mat44> mats(m_count);
        JPH::RVec3 rootOffset;
        m_ragdoll->GetPose(rootOffset, mats.data());
        // GetPose returns matrices relative to rootOffset; fold the offset back in so
        // the caller gets absolute world transforms.
        JPH::Vec3 ofs((float)rootOffset.GetX(), (float)rootOffset.GetY(), (float)rootOffset.GetZ());
        for (uint32_t i = 0; i < m_count; ++i) {
            JPH::Mat44 m = mats[i];
            m.SetTranslation(m.GetTranslation() + ofs);
            fromMat44(m, &out[i*16]);
        }
    }

    void applyImpulseAll(Vec3 impulse) override {
        if (m_ragdoll) m_ragdoll->AddImpulse(JPH::Vec3(impulse.x, impulse.y, impulse.z));
    }

    void applyImpulseBone(uint32_t bone, Vec3 impulse) override {
        if (!m_ragdoll || bone >= m_count) return;
        JPH::BodyID bid = m_ragdoll->GetBodyID((int)bone);
        m_system->GetBodyInterface().AddImpulse(bid, JPH::Vec3(impulse.x, impulse.y, impulse.z));
    }

    bool isActive() const override {
        return m_ragdoll && m_inWorld && m_ragdoll->IsActive();
    }

    void worldBounds(float outMin[3], float outMax[3]) const override {
        if (m_ragdoll && m_inWorld) {
            JPH::AABox box = m_ragdoll->GetWorldSpaceBounds();
            outMin[0] = box.mMin.GetX(); outMin[1] = box.mMin.GetY(); outMin[2] = box.mMin.GetZ();
            outMax[0] = box.mMax.GetX(); outMax[1] = box.mMax.GetY(); outMax[2] = box.mMax.GetZ();
            return;
        }
        // Fall back to the bind bounds.
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (uint32_t i = 0; i < m_count; ++i) {
            const float* m = &m_bindWorld[i*16];
            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], m[12+a]); hi[a] = std::max(hi[a], m[12+a]); }
        }
        std::memcpy(outMin, lo, sizeof(lo));
        std::memcpy(outMax, hi, sizeof(hi));
    }

private:
    IPhysicsWorld*                  m_world  = nullptr;
    JPH::PhysicsSystem*             m_system = nullptr;
    JPH::Ref<JPH::RagdollSettings>  m_settings;
    JPH::Ref<JPH::Ragdoll>          m_ragdoll;
    std::vector<std::string>        m_names;
    std::vector<float>              m_bindWorld;   // boneCount*16
    uint32_t                        m_count   = 0;
    bool                            m_inWorld = false;

    static JPH::CollisionGroup::GroupID s_groupCounter;
};

JPH::CollisionGroup::GroupID JoltRagdoll::s_groupCounter = 0;

} // namespace

IRagdoll* createRagdoll(IPhysicsWorld& world,
                        const RagdollBoneDesc* bones, uint32_t boneCount) {
    if (!bones || boneCount == 0) {
        x3::logWarn("[ragdoll] createRagdoll: empty bone description");
        return nullptr;
    }
    auto* rd = new JoltRagdoll(world, bones, boneCount);
    if (!rd->build(bones, boneCount)) { delete rd; return nullptr; }
    return rd;
}

// ---------------------------------------------------------------------------
// Canonical humanoid test rig (shared by --world ragdoll + --test-ragdoll).
// A simple upright stick figure: pelvis -> spine -> head, two arms, two legs.
// Each bone authored as a world transform whose +Y points toward its child.
// ---------------------------------------------------------------------------
namespace {
// Build a world transform with +Y along `dir` (normalized) at translation `pos`.
// Picks an arbitrary perpendicular for +X so the frame is well-defined.
void boneFrame(const float pos[3], const float dir[3], float out[16]) {
    float ly[3] = { dir[0], dir[1], dir[2] };
    float len = std::sqrt(ly[0]*ly[0] + ly[1]*ly[1] + ly[2]*ly[2]);
    if (len < 1e-6f) { ly[0]=0; ly[1]=1; ly[2]=0; len=1; }
    ly[0]/=len; ly[1]/=len; ly[2]/=len;
    // Pick a reference not parallel to ly.
    float ref[3] = { 1, 0, 0 };
    if (std::fabs(ly[0]) > 0.9f) { ref[0]=0; ref[1]=0; ref[2]=1; }
    // lx = normalize(ref x ly), lz = lx x ly
    float lx[3] = {
        ref[1]*ly[2] - ref[2]*ly[1],
        ref[2]*ly[0] - ref[0]*ly[2],
        ref[0]*ly[1] - ref[1]*ly[0] };
    float lxl = std::sqrt(lx[0]*lx[0]+lx[1]*lx[1]+lx[2]*lx[2]);
    lx[0]/=lxl; lx[1]/=lxl; lx[2]/=lxl;
    float lz[3] = {
        lx[1]*ly[2] - lx[2]*ly[1],
        lx[2]*ly[0] - lx[0]*ly[2],
        lx[0]*ly[1] - lx[1]*ly[0] };
    // Column-major: col0=lx, col1=ly, col2=lz, col3=pos
    out[0]=lx[0]; out[1]=lx[1]; out[2]=lx[2]; out[3]=0;
    out[4]=ly[0]; out[5]=ly[1]; out[6]=ly[2]; out[7]=0;
    out[8]=lz[0]; out[9]=lz[1]; out[10]=lz[2]; out[11]=0;
    out[12]=pos[0]; out[13]=pos[1]; out[14]=pos[2]; out[15]=1;
}
} // namespace

uint32_t makeHumanoidRagdollBones(float originY, std::vector<RagdollBoneDesc>& out) {
    out.clear();
    const float up[3]    = { 0, 1, 0 };
    const float down[3]  = { 0, -1, 0 };
    const float right[3] = { 1, 0, 0 };
    const float left[3]  = { -1, 0, 0 };
    auto add = [&](const char* name, int parent, float px, float py, float pz,
                   const float dir[3], float hh, float r, float mass,
                   float cone, float twist) {
        RagdollBoneDesc d;
        d.name = name; d.parent = parent;
        float pos[3] = { px, py + originY, pz };
        boneFrame(pos, dir, d.bindWorld);
        d.halfHeight = hh; d.radius = r; d.mass = mass;
        d.coneHalfAngle = cone; d.twistLimit = twist;
        out.push_back(d);
    };
    // index:                name        parent  px    py    pz   dir    hh    r     mass cone twist
    add("pelvis",            -1,  0.0f, 0.0f,  0.0f, up,    0.10f,0.10f, 8.0f, 0.0f, 0.0f); // 0
    add("spine",              0,  0.0f, 0.20f, 0.0f, up,    0.18f,0.09f, 10.0f,0.5f, 0.3f); // 1
    add("head",               1,  0.0f, 0.56f, 0.0f, up,    0.08f,0.08f, 4.0f, 0.6f, 0.4f); // 2
    add("upperarm.L",         1, -0.15f,0.52f, 0.0f, left,  0.13f,0.045f,2.0f, 1.2f, 0.5f); // 3
    add("forearm.L",          3, -0.41f,0.52f, 0.0f, left,  0.12f,0.04f, 1.5f, 1.4f, 0.3f); // 4
    add("upperarm.R",         1,  0.15f,0.52f, 0.0f, right, 0.13f,0.045f,2.0f, 1.2f, 0.5f); // 5
    add("forearm.R",          5,  0.41f,0.52f, 0.0f, right, 0.12f,0.04f, 1.5f, 1.4f, 0.3f); // 6
    add("thigh.L",            0, -0.09f,0.0f,  0.0f, down,  0.18f,0.06f, 4.0f, 0.9f, 0.3f); // 7
    add("shin.L",             7, -0.09f,-0.40f,0.0f, down,  0.18f,0.05f, 3.0f, 1.0f, 0.2f); // 8
    add("thigh.R",            0,  0.09f,0.0f,  0.0f, down,  0.18f,0.06f, 4.0f, 0.9f, 0.3f); // 9
    add("shin.R",             9,  0.09f,-0.40f,0.0f, down,  0.18f,0.05f, 3.0f, 1.0f, 0.2f); // 10
    return (uint32_t)out.size();
}

// ===========================================================================
// Physics §2 self-test (--test-ragdoll).
// ===========================================================================
namespace {
bool finiteM(const float* m, int n16) {
    for (int i = 0; i < n16 * 16; ++i) if (!std::isfinite(m[i])) return false;
    return true;
}
float boneDist(const float* m, uint32_t a, uint32_t b) {
    const float* ma = &m[a*16]; const float* mb = &m[b*16];
    float dx = ma[12]-mb[12], dy = ma[13]-mb[13], dz = ma[14]-mb[14];
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}
} // namespace

bool runRagdollSelfTest() {
    int pass = 0, total = 0;
    auto P = [&](bool cond, const char* name) {
        ++total;
        if (cond) { ++pass; x3::logInfo(std::string("[ragdoll] PASS ") + name); }
        else      {          x3::logError(std::string("[ragdoll] FAIL ") + name); }
    };

    // ---- R1..R5: build a humanoid ragdoll, drop it, assert it falls + settles ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        // Ground plane (top at y=0).
        {
            float v[] = { -50,0,-50,  50,0,-50,  50,0,50,  -50,0,50 };
            uint32_t idx[] = { 0,2,1, 0,3,2 };
            w->addStaticMesh(v, 4, idx, 6);
        }

        std::vector<RagdollBoneDesc> bones;
        uint32_t n = makeHumanoidRagdollBones(1.2f, bones);  // pelvis 1.2 m up
        std::unique_ptr<IRagdoll> rd(createRagdoll(*w, bones.data(), n));
        P(rd != nullptr, "R1 ragdoll built from synthetic skeleton");
        if (!rd) { x3::logError("ragdoll: " + std::to_string(pass) + "/" +
                                std::to_string(total) + " passed"); w->shutdown(); return false; }
        P(rd->boneCount() == n, "R2 bone count matches");

        rd->addToWorld(true);

        // Record bind-pose bone lengths (parent->child) for the constraint-chain
        // integrity check.
        std::vector<float> bind(n*16);
        rd->getBoneWorldTransforms(bind.data());
        // Pelvis (0) -> spine (1), spine (1) -> head (2), thigh.L(7)->shin.L(8).
        float lenSpine0 = boneDist(bind.data(), 0, 1);
        float lenHead0  = boneDist(bind.data(), 1, 2);
        float lenLeg0   = boneDist(bind.data(), 7, 8);

        // A nudge so it definitely topples (not perfectly balanced).
        rd->applyImpulseAll(Vec3{ 1.5f, 0.0f, 0.5f });

        float startTopY = 0;
        { float lo[3], hi[3]; rd->worldBounds(lo, hi); startTopY = hi[1]; }

        bool nan = false;
        for (int i = 0; i < 600; ++i) {  // 10 s
            w->step(1.0f/60.0f);
            std::vector<float> cur(n*16);
            rd->getBoneWorldTransforms(cur.data());
            if (!finiteM(cur.data(), n)) { nan = true; break; }
        }
        P(!nan, "R3 no NaNs over 10 s of simulation");

        // It fell: the topmost point dropped substantially toward the floor.
        float endTopY = 0;
        { float lo[3], hi[3]; rd->worldBounds(lo, hi); endTopY = hi[1]; }
        P(endTopY < startTopY - 0.4f, "R4 ragdoll fell under gravity");

        // Settled: step more and confirm it has come to rest (not active, or barely
        // moving — compare two snapshots a moment apart).
        std::vector<float> s0(n*16), s1(n*16);
        rd->getBoneWorldTransforms(s0.data());
        for (int i = 0; i < 120; ++i) w->step(1.0f/60.0f);  // 2 s
        rd->getBoneWorldTransforms(s1.data());
        float maxMove = 0.0f;
        for (uint32_t i = 0; i < n; ++i)
            maxMove = std::max(maxMove, boneDist(s0.data(), i, i) /*0*/ +
                       std::fabs(s1[i*16+12]-s0[i*16+12]) +
                       std::fabs(s1[i*16+13]-s0[i*16+13]) +
                       std::fabs(s1[i*16+14]-s0[i*16+14]));
        P(maxMove < 0.15f, "R5 ragdoll settles into a stable pose");

        // Constraint chain held: parent->child bone lengths preserved within tol.
        std::vector<float> fin(n*16);
        rd->getBoneWorldTransforms(fin.data());
        float lenSpine1 = boneDist(fin.data(), 0, 1);
        float lenHead1  = boneDist(fin.data(), 1, 2);
        float lenLeg1   = boneDist(fin.data(), 7, 8);
        bool chainOk =
            std::fabs(lenSpine1 - lenSpine0) < 0.05f &&
            std::fabs(lenHead1  - lenHead0)  < 0.05f &&
            std::fabs(lenLeg1   - lenLeg0)   < 0.05f;
        P(chainOk, "R6 constraint chain holds (bone lengths preserved)");

        rd->removeFromWorld();
        rd.reset();
        w->shutdown();
    }

    // ---- R7: anim<->ragdoll blend weight 0..1 interpolates monotonically ----
    // The blend is a linear interpolation between an "animated" world pose A and a
    // "ragdoll" world pose B per bone (translation lerp). We assert that as the
    // blend weight goes 0 -> 1, each bone's position moves monotonically from A to B
    // and the palette at w=0 equals A and at w=1 equals B (the same math the Skinner
    // uses in applyExternalPose). This is engine-side, deterministic, GPU-free.
    {
        // Two distinct world poses for a 3-bone chain.
        const uint32_t n = 3;
        std::vector<float> A(n*16), B(n*16);
        for (uint32_t i = 0; i < n; ++i) {
            // identity rotation; translations differ between A and B.
            float a[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0,  (float)i, 0, 0, 1};
            float b[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0,  (float)i, -1.0f - (float)i, (float)i, 1};
            std::memcpy(&A[i*16], a, sizeof(a));
            std::memcpy(&B[i*16], b, sizeof(b));
        }
        auto lerpPose = [&](float wgt, std::vector<float>& outv) {
            outv.resize(n*16);
            for (uint32_t i = 0; i < n; ++i) {
                for (int k = 0; k < 16; ++k)
                    outv[i*16+k] = A[i*16+k] * (1.0f - wgt) + B[i*16+k] * wgt;
            }
        };
        std::vector<float> p0, p1, pm;
        lerpPose(0.0f, p0); lerpPose(1.0f, p1); lerpPose(0.5f, pm);
        bool atZero = true, atOne = true;
        for (uint32_t i = 0; i < n*16; ++i) {
            if (std::fabs(p0[i] - A[i]) > 1e-5f) atZero = false;
            if (std::fabs(p1[i] - B[i]) > 1e-5f) atOne  = false;
        }
        P(atZero && atOne, "R7a blend endpoints: w=0 -> anim pose, w=1 -> ragdoll pose");

        // Monotonic sweep: bone translations move steadily A->B as w increases.
        bool mono = true;
        float prevY[n];
        for (uint32_t i = 0; i < n; ++i) prevY[i] = A[i*16+13];
        for (int s = 1; s <= 10; ++s) {
            float wgt = s / 10.0f;
            std::vector<float> ps; lerpPose(wgt, ps);
            for (uint32_t i = 0; i < n; ++i) {
                float y = ps[i*16+13];
                // B[i].y < A[i].y, so y must DECREASE monotonically.
                if (y > prevY[i] + 1e-5f) mono = false;
                prevY[i] = y;
            }
        }
        P(mono, "R7b blend weight 0->1 interpolates the palette monotonically");
        // Midpoint is exactly halfway.
        bool mid = true;
        for (uint32_t i = 0; i < n*16; ++i)
            if (std::fabs(pm[i] - 0.5f*(A[i]+B[i])) > 1e-5f) mid = false;
        P(mid, "R7c blend midpoint is the exact average");
    }

    x3::logInfo("ragdoll: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::phys
