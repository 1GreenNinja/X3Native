// Physics World + Character Controller implementation — M3 (clean-room).
// Spec: specs/M3-physics-world.spec.md
//
// Jolt Physics (MIT). All JPH:: types are confined to this translation unit;
// the public interface (IPhysicsWorld.h) is plain structs + opaque BodyId.
//
// Structure follows Jolt's official HelloWorld + CharacterVirtual samples:
//   RegisterDefaultAllocator -> Factory::sInstance -> RegisterTypes ->
//   TempAllocatorImpl + JobSystemThreadPool + a PhysicsSystem with our
//   BroadPhaseLayerInterface / ObjectVsBroadPhaseLayerFilter / ObjectLayerPairFilter.
//
// Verified via runPhysicsSelfTest() (acceptance tests T1-T8).

#include "IPhysicsWorld.h"
#include "../core/x3_log.h"
#include "../core/IJobSystem.h"
#include "JoltJobBridge.h"

// --- Jolt ---
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace x3::phys {

namespace {

// ---------------------------------------------------------------------------
// Jolt trace / assert hooks -> engine log
// ---------------------------------------------------------------------------
void joltTrace(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    x3::logInfo(std::string("[jolt] ") + buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssertFailed(const char* expr, const char* msg, const char* file, JPH::uint line) {
    x3::logError(std::string("[jolt-assert] ") + file + ":" + std::to_string(line) +
                 " (" + expr + ") " + (msg ? msg : ""));
    return true; // breakpoint
}
#endif

// ---------------------------------------------------------------------------
// Layers
//
// Spec Layer enum -> Jolt ObjectLayer (one per enum) + BroadPhaseLayer
// (NON_MOVING for Static/Trigger, MOVING for the rest). The collision matrix:
//   Static    : collides with everything dynamic-ish (queried, not a mover)
//   Dynamic   : collides with Static, Dynamic, Player, Enemy
//   Player    : collides with Static, Dynamic, Enemy, Trigger
//   Enemy     : collides with Static, Dynamic, Player, Enemy, Projectile, Trigger
//   Projectile: collides with Static, Enemy (NOT its owner -> owner filtering is
//               done per-body, see ProjectileFilter); never with Player here.
//   Trigger   : sensor only; collides (for overlap detection) with Player/Enemy/Dynamic
// ---------------------------------------------------------------------------
namespace ObjLayers {
    static constexpr JPH::ObjectLayer Static     = 0;
    static constexpr JPH::ObjectLayer Dynamic    = 1;
    static constexpr JPH::ObjectLayer Player     = 2;
    static constexpr JPH::ObjectLayer Enemy      = 3;
    static constexpr JPH::ObjectLayer Projectile = 4;
    static constexpr JPH::ObjectLayer Trigger    = 5;
    static constexpr JPH::ObjectLayer Count      = 6;
}

namespace BPLayers {
    static constexpr JPH::BroadPhaseLayer NonMoving(0);
    static constexpr JPH::BroadPhaseLayer Moving(1);
    static constexpr JPH::uint Count = 2;
}

JPH::ObjectLayer toObjLayer(Layer l) {
    switch (l) {
        case Layer::Static:     return ObjLayers::Static;
        case Layer::Dynamic:    return ObjLayers::Dynamic;
        case Layer::Player:     return ObjLayers::Player;
        case Layer::Enemy:      return ObjLayers::Enemy;
        case Layer::Projectile: return ObjLayers::Projectile;
        case Layer::Trigger:    return ObjLayers::Trigger;
    }
    return ObjLayers::Static;
}

// Object-layer pair collision matrix.
bool objectLayersCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) {
    // Symmetric helper.
    auto pair = [&](JPH::ObjectLayer x, JPH::ObjectLayer y) {
        return (a == x && b == y) || (a == y && b == x);
    };
    using namespace ObjLayers;
    // Trigger: overlap with Player/Enemy/Dynamic only.
    if (a == Trigger || b == Trigger) {
        return pair(Trigger, Player) || pair(Trigger, Enemy) || pair(Trigger, Dynamic);
    }
    // Static: never collides with Static.
    if (a == Static && b == Static) return false;
    // Projectile: Static, Enemy only (owner filtered separately, never Player/Dynamic/Projectile).
    if (a == Projectile || b == Projectile) {
        return pair(Projectile, Static) || pair(Projectile, Enemy);
    }
    // Everything else: Static/Dynamic/Player/Enemy all collide with each other.
    return true;
}

// Ray/query mask predicate. The `mask` names the layer(s) a query targets, so a
// ray with the Static mask must hit Static geometry (unlike body-vs-body where
// Static<->Static never collides). A query hits a body when the masked layer is
// the same layer, or when the two layers would collide per the matrix (so e.g.
// a Projectile-mask probe sees Static+Enemy but passes through Player).
bool queryHitsLayer(JPH::ObjectLayer mask, JPH::ObjectLayer bodyLayer) {
    if (mask == bodyLayer) return true;
    return objectLayersCollide(mask, bodyLayer);
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        m_map[ObjLayers::Static]     = BPLayers::NonMoving;
        m_map[ObjLayers::Dynamic]    = BPLayers::Moving;
        m_map[ObjLayers::Player]     = BPLayers::Moving;
        m_map[ObjLayers::Enemy]      = BPLayers::Moving;
        m_map[ObjLayers::Projectile] = BPLayers::Moving;
        m_map[ObjLayers::Trigger]    = BPLayers::NonMoving;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::Count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        JPH_ASSERT(l < ObjLayers::Count);
        return m_map[l];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return (l == BPLayers::NonMoving) ? "NON_MOVING" : "MOVING";
    }
#endif
private:
    JPH::BroadPhaseLayer m_map[ObjLayers::Count];
};

class ObjectVsBroadPhaseFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override {
        // Static/Trigger live in NonMoving. Their object layers should still be
        // tested against the right broadphase layers based on the matrix.
        using namespace ObjLayers;
        switch (obj) {
            case Static:     return bp == BPLayers::Moving;     // static only cares about movers
            case Trigger:    return bp == BPLayers::Moving;     // trigger only senses movers
            case Dynamic:    return true;
            case Player:     return true;
            case Enemy:      return true;
            case Projectile: return true;                       // Static(NonMoving)+Enemy(Moving)
            default:         return true;
        }
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return objectLayersCollide(a, b);
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline JPH::Vec3 toJ(Vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline JPH::RVec3 toRJ(Vec3 v) { return JPH::RVec3(v.x, v.y, v.z); }
inline Vec3 fromJ(JPH::Vec3Arg v) { return Vec3{ v.GetX(), v.GetY(), v.GetZ() }; }
inline Vec3 fromRJ(JPH::RVec3Arg v) {
    return Vec3{ (float)v.GetX(), (float)v.GetY(), (float)v.GetZ() };
}

constexpr float kFixedDt = 1.0f / 60.0f;

// One-shot global Jolt registration (Factory/RegisterTypes are process-global).
struct JoltGlobalInit {
    JoltGlobalInit() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = joltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssertFailed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltGlobalInit() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};
void ensureJoltGlobals() {
    static JoltGlobalInit s_init; // constructed once, torn down at process exit
    (void)s_init;
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
struct CharData {
    std::unique_ptr<JPH::CharacterVirtual> ch;
    float radius = 0, height = 0;
    JPH::Vec3 desiredHoriz = JPH::Vec3::sZero(); // set by moveCharacter, applied in step
    float vy = 0.0f;                             // integrated vertical velocity
};

class JoltPhysicsWorld final : public IPhysicsWorld {
public:
    bool init() override {
        ensureJoltGlobals();

        m_temp = std::make_unique<JPH::TempAllocatorImpl>(64 * 1024 * 1024);
        // Leave one core for the main thread.
        int hw = (int)std::thread::hardware_concurrency();
        int threads = hw > 1 ? hw - 1 : 1;

        // Slice 41 (D-JOB): physics shares the ONE engine scheduler instead of
        // spinning up a private JobSystemThreadPool. We own + bring up an
        // x3::jobs::IJobSystem and route Jolt's jobs through JoltJobBridge.
        // GetMaxConcurrency reports the worker count so Jolt parallelises the
        // same way it would with its own pool.
        m_engineJobs.reset(x3::jobs::createJobSystem());
        m_engineJobs->init(threads);
        m_jobs = std::make_unique<JoltJobBridge>(
            m_engineJobs.get(), JPH::cMaxPhysicsBarriers, threads);

        const JPH::uint cMaxBodies = 65536;
        const JPH::uint cNumBodyMutexes = 0;
        const JPH::uint cMaxBodyPairs = 65536;
        const JPH::uint cMaxContactConstraints = 20480;

        m_system = std::make_unique<JPH::PhysicsSystem>();
        m_system->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
                       m_bpLayers, m_objVsBp, m_objPair);
        m_system->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

        m_contactListener = std::make_unique<TriggerContactListener>(this);
        m_system->SetContactListener(m_contactListener.get());

        m_accumulator = 0.0f;
        return true;
    }

    void shutdown() override {
        // Release characters first (they reference bodies/shapes).
        m_chars.clear();
        if (m_system) {
            JPH::BodyInterface& bi = m_system->GetBodyInterface();
            for (auto& kv : m_bodies) {
                bi.RemoveBody(kv.second);
                bi.DestroyBody(kv.second);
            }
        }
        m_bodies.clear();
        m_layerOf.clear();
        m_ownerOf.clear();
        m_triggerSet.clear();
        m_contactListener.reset();
        m_system.reset();
        m_jobs.reset();        // bridge references m_engineJobs -> destroy it first
        m_engineJobs.reset();  // joins engine worker threads
        m_temp.reset();
    }

    void step(float dtSeconds) override {
        if (dtSeconds <= 0.0f) return;
        m_accumulator += dtSeconds;
        // Cap to avoid spiral-of-death; max ~0.25s of catch-up.
        if (m_accumulator > 0.25f) m_accumulator = 0.25f;
        while (m_accumulator >= kFixedDt) {
            stepCharacters(kFixedDt);
            m_system->Update(kFixedDt, /*collisionSteps*/1, m_temp.get(), m_jobs.get());
            m_accumulator -= kFixedDt;
        }
    }

    BodyId addStaticMesh(const float* verts, uint32_t vcount,
                         const uint32_t* indices, uint32_t icount) override {
        if (!verts || vcount == 0 || !indices || icount < 3) {
            x3::logWarn("[phys] addStaticMesh: empty/invalid mesh");
            return {};
        }
        if (vcount > 1000000u)
            x3::logWarn("[phys] addStaticMesh: very large mesh (" +
                        std::to_string(vcount) + " verts)");

        JPH::VertexList vlist;
        vlist.reserve(vcount);
        for (uint32_t i = 0; i < vcount; ++i)
            vlist.push_back(JPH::Float3(verts[i*3+0], verts[i*3+1], verts[i*3+2]));

        JPH::IndexedTriangleList tris;
        tris.reserve(icount / 3);
        for (uint32_t i = 0; i + 2 < icount; i += 3)
            tris.push_back(JPH::IndexedTriangle(indices[i], indices[i+1], indices[i+2], 0));

        JPH::MeshShapeSettings settings(vlist, tris);
        settings.SetEmbedded();
        JPH::ShapeSettings::ShapeResult res = settings.Create();
        if (res.HasError()) {
            x3::logError(std::string("[phys] mesh shape error: ") + res.GetError().c_str());
            return {};
        }
        return createBody(res.Get(), Vec3{}, 0.0f, Layer::Static);
    }

    BodyId addBox(Vec3 halfExtents, Vec3 pos, float mass, Layer layer) override {
        JPH::Vec3 he(halfExtents.x, halfExtents.y, halfExtents.z);
        // Box convex radius must be < smallest half-extent.
        float minHe = std::min({ he.GetX(), he.GetY(), he.GetZ() });
        float conv = std::min(0.05f, minHe * 0.1f);
        JPH::BoxShapeSettings ss(he, conv);
        ss.SetEmbedded();
        auto res = ss.Create();
        if (res.HasError()) { x3::logError(std::string("[phys] box: ") + res.GetError().c_str()); return {}; }
        return createBody(res.Get(), pos, mass, layer);
    }

    BodyId addSphere(float radius, Vec3 pos, float mass, Layer layer) override {
        JPH::SphereShapeSettings ss(radius);
        ss.SetEmbedded();
        auto res = ss.Create();
        if (res.HasError()) { x3::logError(std::string("[phys] sphere: ") + res.GetError().c_str()); return {}; }
        return createBody(res.Get(), pos, mass, layer);
    }

    void removeBody(BodyId id) override {
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) {
            if (!m_warnedStaleRemove) {
                x3::logWarn("[phys] removeBody: invalid/stale id (warned once)");
                m_warnedStaleRemove = true;
            }
            return;
        }
        JPH::BodyInterface& bi = m_system->GetBodyInterface();
        bi.RemoveBody(it->second);
        bi.DestroyBody(it->second);
        m_bodies.erase(it);
        m_layerOf.erase(id.id);
        m_ownerOf.erase(id.id);
        m_triggerSet.erase(id.id);
    }

    void setBodyPosition(BodyId id, Vec3 p) override {
        auto cit = m_chars.find(id.id);
        if (cit != m_chars.end()) { cit->second.ch->SetPosition(toRJ(p)); return; }
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        m_system->GetBodyInterface().SetPosition(it->second, toRJ(p), JPH::EActivation::Activate);
    }

    Vec3 getBodyPosition(BodyId id) const override {
        auto cit = m_chars.find(id.id);
        if (cit != m_chars.end()) return fromRJ(cit->second.ch->GetPosition());
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return {};
        return fromRJ(m_system->GetBodyInterface().GetPosition(it->second));
    }

    void applyImpulse(BodyId id, Vec3 impulse) override {
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        m_system->GetBodyInterface().AddImpulse(it->second, toJ(impulse));
    }

    // ---- Orientation (D-phys) ----
    // CONVENTIONS.md quaternion order is (x,y,z,w); JPH::Quat is also xyzw
    // (constructor Quat(x,y,z,w), GetX/Y/Z/W, sIdentity()=={0,0,0,1}). So the
    // boundary below is a straight component copy in both directions — no reorder.
    void getBodyRotation(BodyId id, float outQuat[4]) const override {
        if (!outQuat) return;
        outQuat[0] = outQuat[1] = outQuat[2] = 0.0f; outQuat[3] = 1.0f; // identity default
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        JPH::Quat q = m_system->GetBodyInterface().GetRotation(it->second);
        outQuat[0] = q.GetX(); outQuat[1] = q.GetY(); outQuat[2] = q.GetZ(); outQuat[3] = q.GetW();
    }

    void setBodyRotation(BodyId id, const float quat[4]) override {
        if (!quat) return;
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        // (x,y,z,w) -> JPH::Quat(x,y,z,w). Normalize defensively; SetRotation
        // expects a unit quaternion.
        JPH::Quat q(quat[0], quat[1], quat[2], quat[3]);
        if (q.LengthSq() < 1e-12f) q = JPH::Quat::sIdentity();
        else                       q = q.Normalized();
        m_system->GetBodyInterface().SetRotation(it->second, q, JPH::EActivation::Activate);
    }

    // ---- Velocities (D-phys) — world-space m/s and rad/s ----
    void setBodyLinearVelocity(BodyId id, const float v[3]) override {
        if (!v) return;
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        m_system->GetBodyInterface().SetLinearVelocity(it->second, JPH::Vec3(v[0], v[1], v[2]));
    }

    void getBodyLinearVelocity(BodyId id, float out[3]) const override {
        if (!out) return;
        out[0] = out[1] = out[2] = 0.0f;
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        JPH::Vec3 v = m_system->GetBodyInterface().GetLinearVelocity(it->second);
        out[0] = v.GetX(); out[1] = v.GetY(); out[2] = v.GetZ();
    }

    void setBodyAngularVelocity(BodyId id, const float v[3]) override {
        if (!v) return;
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        m_system->GetBodyInterface().SetAngularVelocity(it->second, JPH::Vec3(v[0], v[1], v[2]));
    }

    void getBodyAngularVelocity(BodyId id, float out[3]) const override {
        if (!out) return;
        out[0] = out[1] = out[2] = 0.0f;
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        JPH::Vec3 v = m_system->GetBodyInterface().GetAngularVelocity(it->second);
        out[0] = v.GetX(); out[1] = v.GetY(); out[2] = v.GetZ();
    }

    // ---- Per-body user tag (D-phys) ----
    void setBodyUserData(BodyId id, uint64_t data) override {
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return;
        m_system->GetBodyInterface().SetUserData(it->second, (JPH::uint64)data);
    }

    uint64_t getBodyUserData(BodyId id) const override {
        auto it = m_bodies.find(id.id);
        if (it == m_bodies.end()) return 0;
        return (uint64_t)m_system->GetBodyInterface().GetUserData(it->second);
    }

    BodyId createCharacter(float radius, float height, Vec3 pos) override {
        // Capsule: total height = cylinder height + 2*radius. "height" param is
        // the cylinder portion's half-height input convention here -> we treat
        // `height` as the standing capsule's full height and derive cylinder.
        float cyl = std::max(0.01f, height - 2.0f * radius);
        float halfCyl = cyl * 0.5f;
        // Shift so the capsule's bottom sits at the character's origin (feet).
        JPH::Ref<JPH::CapsuleShape> caps = new JPH::CapsuleShape(halfCyl, radius);
        JPH::RotatedTranslatedShapeSettings rts(
            JPH::Vec3(0, halfCyl + radius, 0), JPH::Quat::sIdentity(), caps);
        rts.SetEmbedded();
        auto shapeRes = rts.Create();
        if (shapeRes.HasError()) {
            x3::logError(std::string("[phys] character shape: ") + shapeRes.GetError().c_str());
            return {};
        }

        JPH::CharacterVirtualSettings settings;
        settings.mShape = shapeRes.Get();
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius * 0.9f);
        settings.mCharacterPadding = 0.02f;

        uint32_t id = m_nextId++;
        CharData cd;
        cd.radius = radius;
        cd.height = height;
        cd.ch.reset(new JPH::CharacterVirtual(&settings, toRJ(pos), JPH::Quat::sIdentity(), 0, m_system.get()));
        m_chars.emplace(id, std::move(cd));
        return BodyId{ id };
    }

    void moveCharacter(BodyId id, Vec3 desiredVelocity, float /*dt*/) override {
        auto it = m_chars.find(id.id);
        if (it == m_chars.end()) return;
        // Record the desired horizontal velocity; vertical is treated as a jump
        // impulse (set vy directly) and otherwise driven by gravity in step().
        it->second.desiredHoriz = JPH::Vec3(desiredVelocity.x, 0.0f, desiredVelocity.z);
        if (desiredVelocity.y > 0.0f) it->second.vy = desiredVelocity.y; // jump
    }

    bool characterGrounded(BodyId id) const override {
        auto it = m_chars.find(id.id);
        if (it == m_chars.end()) return false;
        return it->second.ch->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    }

    RayHit rayCast(Vec3 origin, Vec3 dir, float maxDist, Layer mask) override {
        RayHit out;
        float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
        if (len < 1e-6f || maxDist <= 0.0f) return out; // zero-length dir -> no hit

        JPH::Vec3 d(dir.x/len, dir.y/len, dir.z/len);
        JPH::RRayCast ray{ toRJ(origin), d * maxDist };

        JPH::RayCastResult hit;
        // Object-layer filter restricts the ray to bodies in layers the mask
        // targets (see queryHitsLayer). Broadphase filter accepts everything;
        // the object-layer filter does the real narrowing.
        JPH::BroadPhaseLayerFilter bpf;
        MaskObjFilter of(mask);
        const JPH::NarrowPhaseQuery& npq = m_system->GetNarrowPhaseQuery();
        if (npq.CastRay(ray, hit, bpf, of)) {
            out.hit = true;
            JPH::RVec3 pt = ray.mOrigin + ray.mDirection * hit.mFraction;
            out.point = fromRJ(pt);
            out.distance = hit.mFraction * maxDist;
            auto bit = m_idOfBody.find(hit.mBodyID.GetIndexAndSequenceNumber());
            if (bit != m_idOfBody.end()) out.body = BodyId{ bit->second };
            // Surface normal at hit point.
            JPH::BodyLockRead lock(m_system->GetBodyLockInterface(), hit.mBodyID);
            if (lock.Succeeded()) {
                const JPH::Body& body = lock.GetBody();
                JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, pt);
                out.normal = fromJ(n);
            }
        }
        return out;
    }

    void setTriggerCallback(TriggerFn fn, void* user) override {
        m_triggerFn = fn;
        m_triggerUser = user;
    }

private:
    // ---- contact listener for trigger overlap (enter/leave) ----
    class TriggerContactListener final : public JPH::ContactListener {
    public:
        explicit TriggerContactListener(JoltPhysicsWorld* w) : m_w(w) {}
        void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                            const JPH::ContactManifold&, JPH::ContactSettings&) override {
            m_w->onContact(a, b, true);
        }
        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            m_w->onContactRemoved(pair.GetBody1ID(), pair.GetBody2ID());
        }
    private:
        JoltPhysicsWorld* m_w;
    };

    // ---- ray filters honoring our layer mask ----
    class MaskObjFilter final : public JPH::ObjectLayerFilter {
    public:
        explicit MaskObjFilter(Layer mask) : m_mask(toObjLayer(mask)) {}
        bool ShouldCollide(JPH::ObjectLayer l) const override {
            return queryHitsLayer(m_mask, l);
        }
    private:
        JPH::ObjectLayer m_mask;
    };

    BodyId createBody(JPH::ShapeRefC shape, Vec3 pos, float mass, Layer layer) {
        JPH::BodyInterface& bi = m_system->GetBodyInterface();

        bool wantsDynamic = (mass > 0.0f) &&
            (layer == Layer::Dynamic || layer == Layer::Player ||
             layer == Layer::Enemy   || layer == Layer::Projectile);
        if (mass <= 0.0f && layer == Layer::Dynamic) {
            x3::logWarn("[phys] dynamic body with mass<=0 -> treated as static");
        }

        JPH::EMotionType motion = wantsDynamic ? JPH::EMotionType::Dynamic
                                               : JPH::EMotionType::Static;
        JPH::ObjectLayer ol = toObjLayer(layer);
        // Static-by-mass override keeps it in a non-moving broadphase slot.
        if (!wantsDynamic && layer == Layer::Dynamic) ol = ObjLayers::Static;

        JPH::BodyCreationSettings bcs(shape, toRJ(pos), JPH::Quat::sIdentity(), motion, ol);
        if (wantsDynamic) {
            bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass = mass;
        }
        bool isTrigger = (layer == Layer::Trigger);
        if (isTrigger) bcs.mIsSensor = true;

        JPH::Body* body = bi.CreateBody(bcs);
        if (!body) { x3::logError("[phys] CreateBody failed (body limit?)"); return {}; }
        bi.AddBody(body->GetID(), wantsDynamic ? JPH::EActivation::Activate
                                               : JPH::EActivation::DontActivate);

        uint32_t id = m_nextId++;
        m_bodies[id] = body->GetID();
        m_idOfBody[body->GetID().GetIndexAndSequenceNumber()] = id;
        m_layerOf[id] = layer;
        if (isTrigger) m_triggerSet.insert(id);
        return BodyId{ id };
    }

    void stepCharacters(float dt) {
        for (auto& kv : m_chars) updateCharacter(kv.second, dt);
    }

    void updateCharacter(CharData& cd, float dt) {
        if (dt <= 0.0f) return;
        JPH::Vec3 g = m_system->GetGravity();
        bool grounded =
            cd.ch->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
        if (grounded && cd.vy <= 0.0f) {
            // Stick to the floor: small downward bias so ExtendedUpdate keeps contact.
            cd.vy = 0.0f;
        } else {
            cd.vy += g.GetY() * dt; // integrate gravity on the vertical axis
        }
        // Compose horizontal (player-driven) + vertical (gravity/jump) velocity.
        JPH::Vec3 v(cd.desiredHoriz.GetX(), cd.vy, cd.desiredHoriz.GetZ());
        cd.ch->SetLinearVelocity(v);

        JPH::CharacterVirtual::ExtendedUpdateSettings up;
        // Allow stepping up small ledges (0.4m) and sticking down stairs.
        up.mStickToFloorStepDown = JPH::Vec3(0, -0.5f, 0);
        up.mWalkStairsStepUp = JPH::Vec3(0, 0.4f, 0);

        JPH::DefaultBroadPhaseLayerFilter bpf =
            m_system->GetDefaultBroadPhaseLayerFilter(ObjLayers::Player);
        JPH::DefaultObjectLayerFilter of =
            m_system->GetDefaultLayerFilter(ObjLayers::Player);

        cd.ch->ExtendedUpdate(dt, m_system->GetGravity(), up, bpf, of, {}, {}, *m_temp);
    }

    void onContact(const JPH::Body& a, const JPH::Body& b, bool entered) {
        // Only care about trigger (sensor) involvement.
        uint32_t ida = idOf(a.GetID()), idb = idOf(b.GetID());
        if (!ida || !idb) return;
        bool aTrig = m_triggerSet.count(ida) != 0;
        bool bTrig = m_triggerSet.count(idb) != 0;
        if (aTrig == bTrig) return; // need exactly one trigger
        uint32_t trig = aTrig ? ida : idb;
        uint32_t other = aTrig ? idb : ida;
        if (m_triggerFn) m_triggerFn(BodyId{trig}, BodyId{other}, entered, m_triggerUser);
    }

    void onContactRemoved(const JPH::BodyID& a, const JPH::BodyID& b) {
        uint32_t ida = idOf(a), idb = idOf(b);
        if (!ida || !idb) return;
        bool aTrig = m_triggerSet.count(ida) != 0;
        bool bTrig = m_triggerSet.count(idb) != 0;
        if (aTrig == bTrig) return;
        uint32_t trig = aTrig ? ida : idb;
        uint32_t other = aTrig ? idb : ida;
        if (m_triggerFn) m_triggerFn(BodyId{trig}, BodyId{other}, false, m_triggerUser);
    }

    uint32_t idOf(const JPH::BodyID& bid) const {
        auto it = m_idOfBody.find(bid.GetIndexAndSequenceNumber());
        return it == m_idOfBody.end() ? 0u : it->second;
    }

    // Jolt state
    std::unique_ptr<JPH::TempAllocatorImpl> m_temp;
    // Slice 41: the engine job system that physics shares, and the JPH::JobSystem
    // adapter that routes Jolt's jobs onto it. m_engineJobs must outlive m_jobs.
    std::unique_ptr<x3::jobs::IJobSystem> m_engineJobs;
    std::unique_ptr<JPH::JobSystem> m_jobs;
    std::unique_ptr<JPH::PhysicsSystem> m_system;
    std::unique_ptr<TriggerContactListener> m_contactListener;
    BPLayerInterfaceImpl m_bpLayers;
    ObjectVsBroadPhaseFilterImpl m_objVsBp;
    ObjectLayerPairFilterImpl m_objPair;

    // Id maps
    uint32_t m_nextId = 1; // 0 is the invalid BodyId
    std::unordered_map<uint32_t, JPH::BodyID> m_bodies;
    std::unordered_map<uint32_t, uint32_t> m_idOfBody; // jolt index+seq -> our id
    std::unordered_map<uint32_t, Layer> m_layerOf;
    std::unordered_map<uint32_t, uint32_t> m_ownerOf;
    std::unordered_map<uint32_t, CharData> m_chars;
    std::unordered_set<uint32_t> m_triggerSet;

    float m_accumulator = 0.0f;
    bool m_warnedStaleRemove = false;

    TriggerFn m_triggerFn = nullptr;
    void* m_triggerUser = nullptr;
};

// ===========================================================================
// Self-test (acceptance tests T1-T8)
// ===========================================================================
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[phys-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[phys-test] FAIL ") + name); }
}

// Build a flat ground plane (two triangles) centered at origin, top at y=0.
BodyId makeGround(IPhysicsWorld* w, float halfSize = 50.0f) {
    float v[] = {
        -halfSize, 0.0f, -halfSize,
         halfSize, 0.0f, -halfSize,
         halfSize, 0.0f,  halfSize,
        -halfSize, 0.0f,  halfSize,
    };
    // Wind CCW so the face normal points +Y (Jolt mesh triangles are one-sided).
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w->addStaticMesh(v, 4, idx, 6);
}

// Trigger callback capture for T7.
struct TrigCapture { int entered = 0; int left = 0; };
void trigCb(BodyId, BodyId, bool entered, void* user) {
    auto* c = static_cast<TrigCapture*>(user);
    if (entered) c->entered++; else c->left++;
}

// Run a deterministic scenario; return final box position. Used by T8.
Vec3 runDeterministic() {
    std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
    w->init();
    makeGround(w.get());
    BodyId b = w->addBox(Vec3{0.5f,0.5f,0.5f}, Vec3{0.3f, 5.0f, -0.2f}, 10.0f, Layer::Dynamic);
    w->applyImpulse(b, Vec3{2.0f, 0.0f, 1.0f});
    for (int i = 0; i < 180; ++i) w->step(kFixedDt);
    Vec3 p = w->getBodyPosition(b);
    w->shutdown();
    return p;
}

bool approx(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

} // namespace

IPhysicsWorld* createPhysicsWorld() { return new JoltPhysicsWorld(); }

bool runPhysicsSelfTest() {
    g_pass = g_fail = 0;

    // ---- T1: falling box rests on ground ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        makeGround(w.get());
        BodyId box = w->addBox(Vec3{0.5f,0.5f,0.5f}, Vec3{0,5,0}, 10.0f, Layer::Dynamic);
        for (int i = 0; i < 240; ++i) w->step(kFixedDt);
        Vec3 p = w->getBodyPosition(box);
        // Rests with center at groundTop(0) + halfExtent(0.5).
        check(approx(p.y, 0.5f, 0.06f), "T1 falling box rests on ground");
        w->shutdown();
    }

    // ---- T2: stack of 10 boxes settles (no explosion/jitter) ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        makeGround(w.get());
        const int N = 10;
        std::vector<BodyId> boxes;
        for (int i = 0; i < N; ++i)
            boxes.push_back(w->addBox(Vec3{0.5f,0.5f,0.5f},
                                      Vec3{0.0f, 0.5f + i * 1.001f, 0.0f}, 5.0f, Layer::Dynamic));
        // settle
        for (int i = 0; i < 480; ++i) w->step(kFixedDt);
        Vec3 p0 = w->getBodyPosition(boxes[0]);
        Vec3 pTop = w->getBodyPosition(boxes[N-1]);
        // record, step a bit more, ensure no drift (settled / sleeping)
        for (int i = 0; i < 60; ++i) w->step(kFixedDt);
        Vec3 pTop2 = w->getBodyPosition(boxes[N-1]);
        bool grounded = approx(p0.y, 0.5f, 0.08f);
        bool stacked  = pTop.y > (N - 2) * 0.9f;          // tower didn't collapse
        bool settled  = approx(pTop.y, pTop2.y, 0.02f);   // no jitter/drift
        check(grounded && stacked && settled, "T2 stack settles + no jitter");
        w->shutdown();
    }

    // ---- T3: character walk / wall stop / grounded ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        makeGround(w.get());
        // a wall at x=3 (static box)
        w->addBox(Vec3{0.25f, 2.0f, 5.0f}, Vec3{3.0f, 2.0f, 0.0f}, 0.0f, Layer::Static);
        BodyId chr = w->createCharacter(0.3f, 1.8f, Vec3{0.0f, 0.05f, 0.0f});
        // settle on ground
        for (int i = 0; i < 30; ++i) { w->moveCharacter(chr, Vec3{0,0,0}, kFixedDt); w->step(kFixedDt); }
        bool groundedStart = w->characterGrounded(chr);
        // walk toward the wall for ~3 seconds
        for (int i = 0; i < 180; ++i) { w->moveCharacter(chr, Vec3{4.0f,0,0}, kFixedDt); w->step(kFixedDt); }
        Vec3 atWall = w->getBodyPosition(chr);
        // wall face is at x = 3 - 0.25 = 2.75; capsule radius 0.3 -> stop near 2.45
        bool stopped = atWall.x < 2.6f && atWall.x > 1.5f;
        check(groundedStart && stopped, "T3 character walk + wall stop + grounded");
        w->shutdown();
    }

    // ---- T4: step up a 0.3m ledge without jumping ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        makeGround(w.get());
        // A 0.3m-high raised platform: near edge at x=1, extends well past where
        // the character will end up (spans x=1..21), top at y=0.3.
        w->addBox(Vec3{10.0f, 0.15f, 5.0f}, Vec3{11.0f, 0.15f, 0.0f}, 0.0f, Layer::Static);
        BodyId chr = w->createCharacter(0.3f, 1.8f, Vec3{0.0f, 0.05f, 0.0f});
        for (int i = 0; i < 30; ++i) { w->moveCharacter(chr, Vec3{0,0,0}, kFixedDt); w->step(kFixedDt); }
        // walk onto the step (controlled distance so we end standing on top)
        for (int i = 0; i < 120; ++i) { w->moveCharacter(chr, Vec3{2.0f,0,0}, kFixedDt); w->step(kFixedDt); }
        Vec3 p = w->getBodyPosition(chr);
        bool climbed = p.y > 0.25f;       // feet up on the 0.3m step
        bool advanced = p.x > 1.5f;       // actually moved onto it
        check(climbed && advanced, "T4 step up 0.3m ledge");
        w->shutdown();
    }

    // ---- T5: raycast hits ground, correct point/normal/distance ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        makeGround(w.get());
        for (int i = 0; i < 2; ++i) w->step(kFixedDt);
        RayHit h = w->rayCast(Vec3{0, 5, 0}, Vec3{0, -1, 0}, 10.0f, Layer::Static);
        bool hit = h.hit;
        bool point = approx(h.point.y, 0.0f, 0.02f);
        bool normal = h.normal.y > 0.9f;             // up normal
        bool dist = approx(h.distance, 5.0f, 0.02f); // from y=5 to y=0
        // zero-length dir -> no hit, no crash
        RayHit z = w->rayCast(Vec3{0,5,0}, Vec3{0,0,0}, 10.0f, Layer::Static);
        check(hit && point && normal && dist && !z.hit, "T5 raycast point/normal/distance");
        w->shutdown();
    }

    // ---- T6: layer filter (projectile vs ray-mask) ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        makeGround(w.get());
        // An enemy body the ray (Enemy mask) should hit.
        w->addBox(Vec3{0.5f,0.5f,0.5f}, Vec3{0.0f, 2.0f, 0.0f}, 0.0f, Layer::Enemy);
        RayHit hitEnemy = w->rayCast(Vec3{0, 6, 0}, Vec3{0,-1,0}, 10.0f, Layer::Enemy);
        // A ray with Projectile mask should NOT hit a Player body (projectile
        // never collides with player in the matrix).
        std::unique_ptr<IPhysicsWorld> w2(createPhysicsWorld());
        w2->init();
        w2->addBox(Vec3{0.5f,0.5f,0.5f}, Vec3{0.0f, 2.0f, 0.0f}, 0.0f, Layer::Player);
        RayHit missPlayer = w2->rayCast(Vec3{0,6,0}, Vec3{0,-1,0}, 10.0f, Layer::Projectile);
        check(hitEnemy.hit && !missPlayer.hit, "T6 layer filter (enemy hit / projectile-vs-player pass)");
        w->shutdown();
        w2->shutdown();
    }

    // ---- T7: trigger enter / leave ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        TrigCapture cap;
        w->setTriggerCallback(trigCb, &cap);
        // a static sensor trigger volume centered at origin
        w->addBox(Vec3{1.0f,1.0f,1.0f}, Vec3{0,1.0f,0}, 0.0f, Layer::Trigger);
        // a dynamic body we move in and then out
        BodyId mover = w->addSphere(0.3f, Vec3{0.0f, 1.0f, 5.0f}, 1.0f, Layer::Dynamic);
        // move it into the trigger
        w->setBodyPosition(mover, Vec3{0.0f, 1.0f, 0.0f});
        for (int i = 0; i < 5; ++i) w->step(kFixedDt);
        bool entered = cap.entered >= 1;
        // move it far out
        w->setBodyPosition(mover, Vec3{0.0f, 1.0f, 20.0f});
        for (int i = 0; i < 5; ++i) w->step(kFixedDt);
        bool left = cap.left >= 1;
        check(entered && left, "T7 trigger enter + leave");
        w->shutdown();
    }

    // ---- T8: determinism (same inputs -> same final positions) ----
    {
        Vec3 a = runDeterministic();
        Vec3 b = runDeterministic();
        bool same = approx(a.x, b.x, 1e-4f) && approx(a.y, b.y, 1e-4f) && approx(a.z, b.z, 1e-4f);
        check(same, "T8 determinism (two identical runs match)");
    }

    // ---- T9 (D-phys): setBodyRotation -> getBodyRotation round-trips (xyzw) ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        // Static body so nothing perturbs the orientation between set and get.
        BodyId b = w->addBox(Vec3{0.5f,0.5f,0.5f}, Vec3{0,2,0}, 0.0f, Layer::Static);
        // A 90-deg yaw about +Y as a unit quaternion (x,y,z,w): (0, sin45, 0, cos45).
        const float s = std::sin(0.7853981634f), c = std::cos(0.7853981634f);
        const float qIn[4] = { 0.0f, s, 0.0f, c };
        w->setBodyRotation(b, qIn);
        float qOut[4] = {0,0,0,0};
        w->getBodyRotation(b, qOut);
        // Quaternions q and -q represent the same rotation; allow the sign flip.
        bool same =
            (approx(qOut[0], qIn[0], 1e-4f) && approx(qOut[1], qIn[1], 1e-4f) &&
             approx(qOut[2], qIn[2], 1e-4f) && approx(qOut[3], qIn[3], 1e-4f)) ||
            (approx(qOut[0],-qIn[0], 1e-4f) && approx(qOut[1],-qIn[1], 1e-4f) &&
             approx(qOut[2],-qIn[2], 1e-4f) && approx(qOut[3],-qIn[3], 1e-4f));
        // Default rotation of a fresh body must read back as identity (w=1).
        BodyId d = w->addBox(Vec3{0.5f,0.5f,0.5f}, Vec3{3,2,0}, 0.0f, Layer::Static);
        float qDef[4] = {9,9,9,9};
        w->getBodyRotation(d, qDef);
        bool ident = approx(qDef[0],0,1e-5f) && approx(qDef[1],0,1e-5f) &&
                     approx(qDef[2],0,1e-5f) && approx(qDef[3],1,1e-5f);
        check(same && ident, "T9 setBodyRotation/getBodyRotation round-trip (xyzw)");
        w->shutdown();
    }

    // ---- T10 (D-phys): linear + angular velocity set/get; angular vel rotates ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        // Gravity-free dynamic body in deep space: a sphere far above ground with no
        // contacts. We zero gravity's effect by checking only the X/Z components and
        // the rotation, which gravity (−Y) does not touch.
        BodyId b = w->addSphere(0.5f, Vec3{0, 100.0f, 0}, 1.0f, Layer::Dynamic);

        // Linear velocity set/get round-trip (immediate, pre-step).
        const float linIn[3] = { 1.5f, 0.0f, -2.5f };
        w->setBodyLinearVelocity(b, linIn);
        float linOut[3] = {0,0,0};
        w->getBodyLinearVelocity(b, linOut);
        bool linRT = approx(linOut[0],1.5f,1e-3f) && approx(linOut[1],0.0f,1e-3f) &&
                     approx(linOut[2],-2.5f,1e-3f);

        // Angular velocity set/get round-trip, then verify it actually rotates the
        // body over time. Spin about +Y at 2 rad/s for ~0.5s (~0.5 rad expected).
        const float angIn[3] = { 0.0f, 2.0f, 0.0f };
        w->setBodyAngularVelocity(b, angIn);
        float angOut[3] = {0,0,0};
        w->getBodyAngularVelocity(b, angOut);
        bool angRT = approx(angOut[0],0.0f,1e-3f) && approx(angOut[1],2.0f,1e-3f) &&
                     approx(angOut[2],0.0f,1e-3f);

        float q0[4]; w->getBodyRotation(b, q0); // identity at start
        // Keep re-asserting angular velocity each step so Jolt's damping/sleeping
        // doesn't bleed it off; 30 steps * 1/60 = 0.5s.
        for (int i = 0; i < 30; ++i) { w->setBodyAngularVelocity(b, angIn); w->step(kFixedDt); }
        float q1[4]; w->getBodyRotation(b, q1);
        // Body actually rotated about Y: the Y component grew well past noise and W
        // dropped below 1. (~0.5 rad about Y -> qY ~ sin(0.25) ~ 0.247.)
        bool rotated = std::fabs(q1[1]) > 0.05f && q1[3] < 0.999f &&
                       std::fabs(q1[1]) > std::fabs(q0[1]) + 0.04f;
        check(linRT && angRT && rotated, "T10 linear/angular velocity set+get + spin");
        w->shutdown();
    }

    // ---- T11 (D-phys): setBodyUserData -> getBodyUserData round-trips ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        BodyId b = w->addBox(Vec3{0.5f,0.5f,0.5f}, Vec3{0,2,0}, 1.0f, Layer::Enemy);
        bool def0 = (w->getBodyUserData(b) == 0ull);            // default is 0
        const uint64_t tag = 0xC0FFEE1234567890ull;             // full 64-bit value
        w->setBodyUserData(b, tag);
        bool rt = (w->getBodyUserData(b) == tag);
        // A second body keeps its own (independent) tag.
        BodyId b2 = w->addBox(Vec3{0.5f,0.5f,0.5f}, Vec3{3,2,0}, 1.0f, Layer::Enemy);
        w->setBodyUserData(b2, 42ull);
        bool indep = (w->getBodyUserData(b) == tag) && (w->getBodyUserData(b2) == 42ull);
        check(def0 && rt && indep, "T11 setBodyUserData/getBodyUserData round-trip");
        w->shutdown();
    }

    x3::logInfo(std::string("[phys-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::phys
