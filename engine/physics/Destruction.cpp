// Destruction system implementation — Subsystem K, tiers T0 + T1 (clean-room).
// Spec: specs/K-gpu-destruction.spec.md. Built from the spec + IPhysicsWorld +
// public Jolt docs only. NO id Tech / RBDOOM source. No JPH:: types here — this
// module talks to physics ONLY through the opaque IPhysicsWorld interface.

#include "Destruction.h"
#include "../core/x3_log.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace x3::phys {

namespace {

// Build the 8 corner points of an axis-aligned box (local space, centered at the
// origin) into `out` (24 floats). A box's corners form a valid (non-coplanar)
// convex hull, so addConvexHull accepts them.
void boxCorners(const float he[3], float out[24]) {
    int k = 0;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                out[k++] = sx * he[0];
                out[k++] = sy * he[1];
                out[k++] = sz * he[2];
            }
}

glm::quat quatFromXYZW(const float q[4]) { return glm::quat(q[3], q[0], q[1], q[2]); } // glm is (w,x,y,z)
void      quatToXYZW(const glm::quat& q, float out[4]) { out[0]=q.x; out[1]=q.y; out[2]=q.z; out[3]=q.w; }

glm::mat4 mat4FromArray(const float m[16]) { return glm::make_mat4(m); }

} // namespace

// ---------------------------------------------------------------------------
// Procedural grid fracture (T1 authoring)
// ---------------------------------------------------------------------------
uint32_t makeGridFractureChunks(float halfX, float halfY, float halfZ,
                                uint32_t nx, uint32_t ny, uint32_t nz,
                                float chunkMass,
                                std::vector<FractureChunkDesc>& out) {
    out.clear();
    nx = std::max(1u, nx); ny = std::max(1u, ny); nz = std::max(1u, nz);
    const float cx = (2.0f * halfX) / nx;   // cell full-size per axis
    const float cy = (2.0f * halfY) / ny;
    const float cz = (2.0f * halfZ) / nz;
    out.reserve((size_t)nx * ny * nz);
    for (uint32_t ix = 0; ix < nx; ++ix)
        for (uint32_t iy = 0; iy < ny; ++iy)
            for (uint32_t iz = 0; iz < nz; ++iz) {
                FractureChunkDesc c;
                c.hullPoints = nullptr;             // box chunk
                c.halfExtents[0] = cx * 0.5f;
                c.halfExtents[1] = cy * 0.5f;
                c.halfExtents[2] = cz * 0.5f;
                // Cell center in the box's local frame (origin at box center).
                c.localOffset[0] = -halfX + cx * (ix + 0.5f);
                c.localOffset[1] = -halfY + cy * (iy + 0.5f);
                c.localOffset[2] = -halfZ + cz * (iz + 0.5f);
                c.localRot[0]=0; c.localRot[1]=0; c.localRot[2]=0; c.localRot[3]=1;
                c.mass = chunkMass;
                out.push_back(c);
            }
    return (uint32_t)out.size();
}

// ---------------------------------------------------------------------------
// DestructibleManager
// ---------------------------------------------------------------------------
DestructibleManager::~DestructibleManager() { shutdown(); }

void DestructibleManager::init(IPhysicsWorld* world, const DestructionTuning& tuning) {
    m_world  = world;
    m_tuning = tuning;
    m_mutationInCallback = false;
    if (m_world) m_world->setContactCallback(&DestructibleManager::contactTrampoline, this);
}

void DestructibleManager::shutdown() {
    if (!m_world) return;
    // Remove all chunk + intact bodies (notify host first so it frees meshes).
    for (uint32_t i = 0; i < (uint32_t)m_chunks.size(); ++i) freeChunkSlot(i);
    m_chunks.clear();
    for (auto& o : m_objects) {
        if (!o.broken && o.intactBody.valid()) m_world->removeBody(o.intactBody);
    }
    m_objects.clear();
    m_assets.clear();
    m_bodyToObject.clear();
    m_pendingBreaks.clear();
    m_events.clear();
    m_world->setContactCallback(nullptr, nullptr);
    m_world = nullptr;
}

void DestructibleManager::setChunkCallbacks(ChunkSpawnFn onSpawn, ChunkDespawnFn onDespawn) {
    m_onSpawn   = std::move(onSpawn);
    m_onDespawn = std::move(onDespawn);
}

FractureAssetId DestructibleManager::loadFractureAsset(const FractureAssetDesc& d) {
    FractureAsset a;
    a.breakImpulse = d.breakImpulse;
    a.breakRelVel  = d.breakRelVel;
    a.chunks.reserve(d.chunkCount);
    a.hullData.reserve(d.chunkCount);
    for (uint32_t i = 0; i < d.chunkCount; ++i) {
        FractureChunkDesc c = d.chunks[i];
        // Own a copy of any hull point data so the desc's pointer stays valid.
        if (c.hullPoints && c.pointCount >= 4) {
            std::vector<float> pts(c.hullPoints, c.hullPoints + (size_t)c.pointCount * 3);
            a.hullData.push_back(std::move(pts));
            c.hullPoints = a.hullData.back().data();
        } else {
            a.hullData.emplace_back();              // keep index parity
            c.hullPoints = nullptr;
        }
        a.chunks.push_back(c);
    }
    // Re-point copied chunk hull pointers (the vector may have reallocated above).
    for (uint32_t i = 0; i < (uint32_t)a.chunks.size(); ++i)
        if (!a.hullData[i].empty()) a.chunks[i].hullPoints = a.hullData[i].data();

    m_assets.push_back(std::move(a));
    return (FractureAssetId)m_assets.size();         // 1-based id
}

DestructibleId DestructibleManager::spawnDestructible(FractureAssetId asset, const float xform[16]) {
    if (!m_world || asset == kInvalidId || asset > m_assets.size() || !xform) return kInvalidId;
    const FractureAsset& a = m_assets[asset - 1];
    if (a.chunks.empty()) return kInvalidId;

    // Build a convex-hull ShapeId per chunk + the local transform array, then a
    // compound shape, then ONE dynamic compound body for the intact object.
    std::vector<ShapeId> partShapes;
    std::vector<float>   localXforms;     // n*16, column-major
    partShapes.reserve(a.chunks.size());
    localXforms.reserve(a.chunks.size() * 16);

    float totalMass = 0.0f;
    for (const FractureChunkDesc& c : a.chunks) {
        ShapeId s;
        if (c.hullPoints && c.pointCount >= 4) {
            s = m_world->addConvexHull(c.hullPoints, c.pointCount);
        }
        if (!s.valid()) {
            // Box chunk (or hull rejected): build a hull from the box corners.
            float corners[24]; boxCorners(c.halfExtents, corners);
            s = m_world->addConvexHull(corners, 8);
        }
        if (!s.valid()) continue;          // skip a hopeless chunk
        partShapes.push_back(s);

        glm::mat4 local = glm::translate(glm::mat4(1.0f),
                              glm::vec3(c.localOffset[0], c.localOffset[1], c.localOffset[2]))
                        * glm::mat4_cast(quatFromXYZW(c.localRot));
        const float* lp = glm::value_ptr(local);
        for (int k = 0; k < 16; ++k) localXforms.push_back(lp[k]);
        totalMass += c.mass;
    }
    if (partShapes.empty()) return kInvalidId;

    ShapeId compound = m_world->addCompound(partShapes.data(), localXforms.data(),
                                            (uint32_t)partShapes.size());
    if (!compound.valid()) return kInvalidId;

    glm::mat4 world = mat4FromArray(xform);
    Vec3 pos{ world[3][0], world[3][1], world[3][2] };
    BodyId body = m_world->addBodyFromShape(compound, pos, std::max(0.1f, totalMass), Layer::Dynamic);
    if (!body.valid()) return kInvalidId;

    // Orient the intact body to the spawn rotation.
    glm::quat wq = glm::quat_cast(glm::mat3(world));
    float wqx[4]; quatToXYZW(glm::normalize(wq), wqx);
    m_world->setBodyRotation(body, wqx);
    // Mark it as destructible (fast "is this a destructible?" check — spec §4a).
    m_world->setBodyUserData(body, kDestructibleMarker);

    Destructible obj;
    obj.asset = asset;
    obj.intactBody = body;
    obj.broken = false;
    std::memcpy(obj.xform, xform, sizeof(obj.xform));
    m_objects.push_back(obj);
    DestructibleId id = (DestructibleId)m_objects.size();   // 1-based

    m_bodyToObject.emplace_back(body.id, id);

    // Report the intact object's chunks to the host for rendering (intact=true).
    // We DON'T create separate bodies for them yet — they live in the compound.
    // The host draws each chunk at parentXform * localOffset.
    Destructible& stored = m_objects[id - 1];
    for (uint32_t i = 0; i < (uint32_t)a.chunks.size(); ++i) {
        const FractureChunkDesc& c = a.chunks[i];
        Chunk ck;
        ck.body  = body;                  // intact chunks share the parent body id
        ck.owner = id;
        ck.halfExtents[0]=c.halfExtents[0]; ck.halfExtents[1]=c.halfExtents[1]; ck.halfExtents[2]=c.halfExtents[2];
        ck.mass = c.mass;
        ck.free = false;
        m_chunks.push_back(ck);
        uint32_t slot = (uint32_t)m_chunks.size() - 1;
        stored.chunkSlots.push_back(slot);

        if (m_onSpawn) {
            ChunkView v{};
            v.body = body; v.owner = id; v.intact = true;
            std::memcpy(v.halfExtents, ck.halfExtents, sizeof(v.halfExtents));
            // Intact chunk world transform = parentWorld * localOffset/rot.
            glm::mat4 local = glm::translate(glm::mat4(1.0f),
                                  glm::vec3(c.localOffset[0], c.localOffset[1], c.localOffset[2]))
                            * glm::mat4_cast(quatFromXYZW(c.localRot));
            glm::mat4 cw = world * local;
            std::memcpy(v.xform, glm::value_ptr(cw), sizeof(v.xform));
            m_onSpawn(v);
        }
    }
    return id;
}

void DestructibleManager::despawn(DestructibleId id) {
    if (id == kInvalidId || id > m_objects.size()) return;
    Destructible& o = m_objects[id - 1];
    // Free all chunk slots owned by this object (free or intact).
    // Collect first (freeChunkSlot may swap-remove and shift indices).
    std::vector<BodyId> freeBodies;
    for (uint32_t i = 0; i < (uint32_t)m_chunks.size(); ) {
        if (m_chunks[i].owner == id) {
            if (m_chunks[i].free) freeBodies.push_back(m_chunks[i].body);
            freeChunkSlot(i);              // swap-removes; do not advance i
        } else {
            ++i;
        }
    }
    if (!o.broken && o.intactBody.valid()) {
        if (m_onDespawn) m_onDespawn(o.intactBody);
        m_world->removeBody(o.intactBody);
        // drop the body->object mapping
        for (auto it = m_bodyToObject.begin(); it != m_bodyToObject.end(); ++it)
            if (it->second == id) { m_bodyToObject.erase(it); break; }
    }
    o.broken = true;
    o.intactBody = BodyId{};
    o.chunkSlots.clear();
}

// ---- Contact callback (post-step, mutation-safe per IPhysicsWorld contract) ----
void DestructibleManager::contactTrampoline(BodyId a, BodyId b, const float point[3],
                                            const float normal[3], float impulse, void* user) {
    static_cast<DestructibleManager*>(user)->onContact(a, b, point, normal, impulse);
}

void DestructibleManager::onContact(BodyId a, BodyId b, const float point[3],
                                    const float normal[3], float impulse) {
    // The IPhysicsWorld contract drains this POST-step (bodies unlocked). We still
    // ONLY enqueue here so all fracture mutation is single-pathed through update().
    // The m_inLockedCallback guard makes that contract testable: if any code path
    // EVER mutated physics (breakObject) while this flag is set, the (c) safety
    // probe trips. It must never trip.
    (void)normal;   // contact normal isn't needed for the impulse/vel break test
    if (!m_world) return;
    m_inLockedCallback = true;
    struct Guard { bool& f; ~Guard(){ f=false; } } guard{ m_inLockedCallback };

    auto consider = [&](BodyId who, BodyId other) {
        DestructibleId id = destructibleOfBody(who);
        if (id == kInvalidId) return;
        const Destructible& o = m_objects[id - 1];
        if (o.broken) return;
        const FractureAsset& asset = m_assets[o.asset - 1];
        // Relative approach speed (recompute from current velocities for the vel test).
        float va[3] = {0,0,0}, vb[3] = {0,0,0};
        m_world->getBodyLinearVelocity(who, va);
        if (other.valid()) m_world->getBodyLinearVelocity(other, vb);
        float rel[3] = { va[0]-vb[0], va[1]-vb[1], va[2]-vb[2] };
        float relSpeed = std::sqrt(rel[0]*rel[0]+rel[1]*rel[1]+rel[2]*rel[2]);
        if (impulse < asset.breakImpulse && relSpeed < asset.breakRelVel) return; // (b) below threshold -> no break

        BreakRequest req;
        req.id = id;
        req.impulse = impulse;
        req.impactPoint[0]=point[0]; req.impactPoint[1]=point[1]; req.impactPoint[2]=point[2];
        // Impart the relative velocity along the contact (the thing that hit it).
        req.impactVel[0]=rel[0]; req.impactVel[1]=rel[1]; req.impactVel[2]=rel[2];
        enqueueBreak(req);
    };
    consider(a, b);
    consider(b, a);
}

void DestructibleManager::enqueueBreak(const BreakRequest& r) {
    // Dedup: one break per object per step.
    for (const auto& q : m_pendingBreaks) if (q.id == r.id) return;
    m_pendingBreaks.push_back(r);
}

bool DestructibleManager::applyHit(const float point[3], const float dir[3], float strength) {
    if (!m_world) return false;
    Vec3 o{ point[0], point[1], point[2] };
    float len = std::sqrt(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    if (len < 1e-6f) return false;
    Vec3 d{ dir[0]/len, dir[1]/len, dir[2]/len };
    // Raycast the dynamic layer; if the hit body is an intact destructible, break it.
    RayHit hit = m_world->rayCast(o, d, 100.0f, Layer::Dynamic);
    if (!hit.hit || !hit.body.valid()) return false;
    DestructibleId id = destructibleOfBody(hit.body);
    if (id == kInvalidId || m_objects[id - 1].broken) return false;
    BreakRequest req;
    req.id = id;
    req.impulse = strength;
    req.impactPoint[0]=hit.point.x; req.impactPoint[1]=hit.point.y; req.impactPoint[2]=hit.point.z;
    req.impactVel[0]=d.x*strength; req.impactVel[1]=d.y*strength; req.impactVel[2]=d.z*strength;
    enqueueBreak(req);
    return true;
}

void DestructibleManager::applyRadialImpulse(const float center[3], float radius, float strength) {
    if (!m_world || radius <= 0.0f) return;
    glm::vec3 c(center[0], center[1], center[2]);
    for (DestructibleId id = 1; id <= (DestructibleId)m_objects.size(); ++id) {
        Destructible& o = m_objects[id - 1];
        if (o.broken || !o.intactBody.valid()) continue;
        Vec3 p = m_world->getBodyPosition(o.intactBody);
        glm::vec3 bp(p.x, p.y, p.z);
        glm::vec3 toBody = bp - c;
        float dist = glm::length(toBody);
        if (dist > radius) continue;
        glm::vec3 outDir = (dist > 1e-4f) ? toBody / dist : glm::vec3(0, 1, 0);
        float falloff = 1.0f - dist / radius;   // linear falloff to the radius edge
        glm::vec3 v = outDir * (strength * falloff);
        BreakRequest req;
        req.id = id;
        req.impulse = strength * falloff;
        req.impactPoint[0]=c.x; req.impactPoint[1]=c.y; req.impactPoint[2]=c.z;
        req.impactVel[0]=v.x; req.impactVel[1]=v.y; req.impactVel[2]=v.z;
        enqueueBreak(req);
    }
}

void DestructibleManager::update(float dt) {
    if (!m_world) return;
    processBreaks();        // the ONLY fracture path (post-step, mutation-safe)
    ageAndReap(dt);
    enforceChunkCap();
}

void DestructibleManager::processBreaks() {
    if (m_pendingBreaks.empty()) return;
    std::vector<BreakRequest> reqs;
    reqs.swap(m_pendingBreaks);
    for (const BreakRequest& r : reqs) {
        if (r.id == kInvalidId || r.id > m_objects.size()) continue;
        Destructible& o = m_objects[r.id - 1];
        if (o.broken || !o.intactBody.valid()) continue;
        breakObject(o, r);
    }
}

void DestructibleManager::breakObject(Destructible& o, const BreakRequest& r) {
    // (c) SAFETY: fracture MUST NOT run while inside the locked contact callback.
    // If this trips, the queue/post-step separation is broken.
    if (m_inLockedCallback) { m_mutationInCallback = true; return; }
    const FractureAsset& a = m_assets[o.asset - 1];

    // Read the intact parent transform + motion BEFORE removing it.
    Vec3 pp = m_world->getBodyPosition(o.intactBody);
    float pq[4]; m_world->getBodyRotation(o.intactBody, pq);
    float plin[3] = {0,0,0}, pang[3] = {0,0,0};
    m_world->getBodyLinearVelocity(o.intactBody, plin);
    m_world->getBodyAngularVelocity(o.intactBody, pang);
    glm::vec3 parentPos(pp.x, pp.y, pp.z);
    glm::quat parentRot = quatFromXYZW(pq);
    glm::vec3 parentLin(plin[0], plin[1], plin[2]);
    glm::vec3 parentAng(pang[0], pang[1], pang[2]);
    glm::vec3 impactVel(r.impactVel[0], r.impactVel[1], r.impactVel[2]);
    glm::vec3 impactPoint(r.impactPoint[0], r.impactPoint[1], r.impactPoint[2]);

    // Remove the parent (spec §4c). Notify host so it frees the intact mesh.
    if (m_onDespawn) m_onDespawn(o.intactBody);
    BodyId parentBody = o.intactBody;
    m_world->removeBody(parentBody);
    for (auto it = m_bodyToObject.begin(); it != m_bodyToObject.end(); ++it)
        if (it->second == (DestructibleId)(&o - m_objects.data()) + 1) { m_bodyToObject.erase(it); break; }
    o.intactBody = BodyId{};
    o.broken = true;

    // Re-tag this object's existing INTACT chunk slots into FREE debris bodies. We
    // reuse the slots (no growth on break) by overwriting them with the new bodies.
    uint32_t spawned = 0;
    for (uint32_t si = 0; si < (uint32_t)o.chunkSlots.size(); ++si) {
        uint32_t slot = o.chunkSlots[si];
        if (slot >= m_chunks.size()) continue;
        Chunk& ck = m_chunks[slot];
        const FractureChunkDesc& cd = (si < a.chunks.size()) ? a.chunks[si] : a.chunks.back();

        // Child world position = parentPos + parentRot * localOffset.
        glm::vec3 localOff(cd.localOffset[0], cd.localOffset[1], cd.localOffset[2]);
        glm::vec3 childPos = parentPos + parentRot * localOff;
        glm::quat childRot = glm::normalize(parentRot * quatFromXYZW(cd.localRot));

        // Build the chunk's own dynamic convex body.
        ShapeId s;
        if (cd.hullPoints && cd.pointCount >= 4) s = m_world->addConvexHull(cd.hullPoints, cd.pointCount);
        if (!s.valid()) { float corners[24]; boxCorners(cd.halfExtents, corners); s = m_world->addConvexHull(corners, 8); }
        if (!s.valid()) { ck.free = true; ck.body = BodyId{}; continue; }

        Vec3 cpos{ childPos.x, childPos.y, childPos.z };
        BodyId cb = m_world->addBodyFromShape(s, cpos, std::max(0.05f, cd.mass), Layer::Dynamic);
        if (!cb.valid()) { ck.free = true; ck.body = BodyId{}; continue; }
        float cqx[4]; quatToXYZW(childRot, cqx);
        m_world->setBodyRotation(cb, cqx);

        // Velocity split (spec §4c / §15):
        //  child linear = parentLin + impact·k + radialFromImpact·(breakImpulse/mass)
        glm::vec3 radialDir = childPos - impactPoint;
        float rl = glm::length(radialDir);
        radialDir = (rl > 1e-4f) ? radialDir / rl : glm::vec3(0, 1, 0);
        float invMass = 1.0f / std::max(0.05f, cd.mass);
        glm::vec3 childLin = parentLin
                           + impactVel * m_tuning.childImpactFactor
                           + radialDir * (r.impulse * m_tuning.radialFactor * invMass);
        //  child angular = parentAng + (chunkPos − parentPos) × impactVel · k
        glm::vec3 lever = childPos - parentPos;
        glm::vec3 childAng = parentAng
                           + glm::cross(lever, impactVel) * (m_tuning.angularScale * 0.1f);
        float clin[3] = { childLin.x, childLin.y, childLin.z };
        float cang[3] = { childAng.x, childAng.y, childAng.z };
        m_world->setBodyLinearVelocity(cb, clin);
        m_world->setBodyAngularVelocity(cb, cang);

        // Re-home the slot as free debris.
        ck.body = cb;
        ck.free = true;
        ck.age  = 0.0f;
        if (m_onSpawn) {
            ChunkView v{};
            v.body = cb; v.owner = (DestructibleId)(&o - m_objects.data()) + 1; v.intact = false;
            std::memcpy(v.halfExtents, ck.halfExtents, sizeof(v.halfExtents));
            glm::mat4 cw = glm::translate(glm::mat4(1.0f), childPos) * glm::mat4_cast(childRot);
            std::memcpy(v.xform, glm::value_ptr(cw), sizeof(v.xform));
            m_onSpawn(v);
        }
        ++spawned;
    }

    // Optimize the broadphase after the churn of remove-1 / add-N.
    m_world->optimizeBroadphase();

    BreakEvent ev;
    ev.id = (DestructibleId)(&o - m_objects.data()) + 1;
    ev.worldPos[0]=parentPos.x; ev.worldPos[1]=parentPos.y; ev.worldPos[2]=parentPos.z;
    ev.impulse = r.impulse;
    ev.childCount = spawned;
    m_events.push_back(ev);
}

void DestructibleManager::ageAndReap(float dt) {
    for (uint32_t i = 0; i < (uint32_t)m_chunks.size(); ) {
        Chunk& ck = m_chunks[i];
        if (ck.free && ck.body.valid()) {
            ck.age += dt;
            bool tooOld = ck.age >= m_tuning.chunkDespawnTime;
            if (tooOld) { freeChunkSlot(i); continue; } // swap-remove; don't advance
        }
        ++i;
    }
}

void DestructibleManager::enforceChunkCap() {
    // Count only FREE debris bodies toward the cap (intact-object chunks share one
    // body and are cheap). Recycle the OLDEST free chunks first.
    auto freeCount = [&]() {
        uint32_t n = 0; for (const Chunk& c : m_chunks) if (c.free && c.body.valid()) ++n; return n;
    };
    while (freeCount() > m_tuning.maxActiveChunks) {
        // Find the oldest free chunk.
        int oldest = -1; float bestAge = -1.0f;
        for (uint32_t i = 0; i < (uint32_t)m_chunks.size(); ++i) {
            const Chunk& c = m_chunks[i];
            if (c.free && c.body.valid() && c.age > bestAge) { bestAge = c.age; oldest = (int)i; }
        }
        if (oldest < 0) break;
        freeChunkSlot((uint32_t)oldest);
    }
}

void DestructibleManager::freeChunkSlot(uint32_t slot) {
    if (slot >= m_chunks.size()) return;
    Chunk& ck = m_chunks[slot];
    if (ck.free && ck.body.valid()) {                 // only free debris own a body to remove
        if (m_onDespawn) m_onDespawn(ck.body);
        m_world->removeBody(ck.body);
    } else if (!ck.free && ck.body.valid() && m_onDespawn) {
        // Intact chunk: the parent body is removed elsewhere; just notify once per
        // unique parent body is handled by despawn(). Here we don't double-remove.
    }
    // Swap-remove to keep m_chunks compact, then fix the owner's slot indices.
    uint32_t last = (uint32_t)m_chunks.size() - 1;
    if (slot != last) {
        m_chunks[slot] = m_chunks[last];
        // Fix any owner referencing the moved slot.
        for (auto& o : m_objects)
            for (auto& s : o.chunkSlots)
                if (s == last) s = slot;
    }
    m_chunks.pop_back();
    // Remove the freed slot index from its owner's list.
    for (auto& o : m_objects) {
        auto& v = o.chunkSlots;
        v.erase(std::remove(v.begin(), v.end(), slot), v.end());
    }
}

uint32_t DestructibleManager::drainBreakEvents(BreakEvent* out, uint32_t maxOut) {
    if (!out || maxOut == 0) { m_events.clear(); return 0; }
    uint32_t n = std::min<uint32_t>(maxOut, (uint32_t)m_events.size());
    for (uint32_t i = 0; i < n; ++i) out[i] = m_events[i];
    m_events.erase(m_events.begin(), m_events.begin() + n);
    return n;
}

void DestructibleManager::forEachActiveChunk(const std::function<void(const ChunkView&)>& fn) const {
    if (!fn || !m_world) return;
    for (DestructibleId id = 1; id <= (DestructibleId)m_objects.size(); ++id) {
        const Destructible& o = m_objects[id - 1];
        if (o.broken) continue;
        if (!o.intactBody.valid()) continue;
        // Intact object: report each chunk at parentWorld * localOffset.
        Vec3 pp = m_world->getBodyPosition(o.intactBody);
        float pq[4]; m_world->getBodyRotation(o.intactBody, pq);
        glm::mat4 pw = glm::translate(glm::mat4(1.0f), glm::vec3(pp.x, pp.y, pp.z))
                     * glm::mat4_cast(quatFromXYZW(pq));
        const FractureAsset& a = m_assets[o.asset - 1];
        for (uint32_t si = 0; si < (uint32_t)o.chunkSlots.size() && si < a.chunks.size(); ++si) {
            const FractureChunkDesc& cd = a.chunks[si];
            const Chunk& ck = m_chunks[o.chunkSlots[si]];
            ChunkView v{};
            v.body = o.intactBody; v.owner = id; v.intact = true;
            std::memcpy(v.halfExtents, ck.halfExtents, sizeof(v.halfExtents));
            glm::mat4 local = glm::translate(glm::mat4(1.0f),
                                  glm::vec3(cd.localOffset[0], cd.localOffset[1], cd.localOffset[2]))
                            * glm::mat4_cast(quatFromXYZW(cd.localRot));
            glm::mat4 cw = pw * local;
            std::memcpy(v.xform, glm::value_ptr(cw), sizeof(v.xform));
            fn(v);
        }
    }
    // Free debris chunks: report each at its own body transform.
    for (const Chunk& ck : m_chunks) {
        if (!ck.free || !ck.body.valid()) continue;
        Vec3 p = m_world->getBodyPosition(ck.body);
        float q[4]; m_world->getBodyRotation(ck.body, q);
        ChunkView v{};
        v.body = ck.body; v.owner = ck.owner; v.intact = false;
        std::memcpy(v.halfExtents, ck.halfExtents, sizeof(v.halfExtents));
        glm::mat4 cw = glm::translate(glm::mat4(1.0f), glm::vec3(p.x, p.y, p.z))
                     * glm::mat4_cast(quatFromXYZW(q));
        std::memcpy(v.xform, glm::value_ptr(cw), sizeof(v.xform));
        fn(v);
    }
}

uint32_t DestructibleManager::destructibleCount() const {
    uint32_t n = 0; for (const auto& o : m_objects) if (!o.broken) ++n; return n;
}

bool DestructibleManager::isBroken(DestructibleId id) const {
    if (id == kInvalidId || id > m_objects.size()) return false;
    return m_objects[id - 1].broken;
}

BodyId DestructibleManager::intactBodyOf(DestructibleId id) const {
    if (id == kInvalidId || id > m_objects.size()) return BodyId{};
    const Destructible& o = m_objects[id - 1];
    return o.broken ? BodyId{} : o.intactBody;
}

DestructibleId DestructibleManager::destructibleOfBody(BodyId b) const {
    if (!b.valid()) return kInvalidId;
    for (const auto& kv : m_bodyToObject) if (kv.first == b.id) return kv.second;
    return kInvalidId;
}

// ===========================================================================
// Self-test (--test-destruction). Cases (a)-(d) per the spec verification gate.
// ===========================================================================
namespace {
int d_pass = 0, d_fail = 0;
void dcheck(bool cond, const char* name) {
    if (cond) { ++d_pass; x3::logInfo(std::string("[destruct-test] PASS ") + name); }
    else      { ++d_fail; x3::logError(std::string("[destruct-test] FAIL ") + name); }
}

// Identity-with-translation column-major 4x4.
void xformAt(float x, float y, float z, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0]=out[5]=out[10]=out[15]=1.0f;
    out[12]=x; out[13]=y; out[14]=z;
}

// Flat ground (mirrors the physics self-test ground).
BodyId destructGround(IPhysicsWorld* w, float halfSize = 50.0f) {
    float v[] = { -halfSize,0,-halfSize,  halfSize,0,-halfSize,  halfSize,0,halfSize,  -halfSize,0,halfSize };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w->addStaticMesh(v, 4, idx, 6);
}

// Build a standard 2x2x2 = 8-chunk crate fracture asset.
FractureAssetId makeCrateAsset(DestructibleManager& mgr, float breakImpulse = 15.0f,
                               float breakRelVel = 8.0f) {
    std::vector<FractureChunkDesc> chunks;
    makeGridFractureChunks(0.5f, 0.5f, 0.5f, 2, 2, 2, /*chunkMass*/1.0f, chunks);
    FractureAssetDesc d{};
    d.chunks = chunks.data();
    d.chunkCount = (uint32_t)chunks.size();
    d.breakImpulse = breakImpulse;
    d.breakRelVel  = breakRelVel;
    return mgr.loadFractureAsset(d);
}
} // namespace

bool runDestructionSelfTest() {
    d_pass = d_fail = 0;
    constexpr float kDt = 1.0f / 60.0f;

    // ---- (a) weapon hit ABOVE threshold -> parent removed + N chunks w/ split vel ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        destructGround(w.get());
        DestructibleManager mgr;
        mgr.init(w.get());
        FractureAssetId asset = makeCrateAsset(mgr);
        float xf[16]; xformAt(0.0f, 2.0f, 0.0f, xf);
        DestructibleId id = mgr.spawnDestructible(asset, xf);
        bool spawned = (id != kInvalidId) && !mgr.isBroken(id);
        uint32_t before = mgr.destructibleCount();

        // Fire a hit straight at the crate from -X with strength well over threshold.
        float pt[3] = { -3.0f, 2.0f, 0.0f };
        float dir[3] = { 1.0f, 0.0f, 0.0f };
        bool queued = mgr.applyHit(pt, dir, /*strength*/50.0f);
        // Hit must be QUEUED, not applied yet (no break before update()).
        bool notYet = !mgr.isBroken(id);
        // Process: step + update (the fracture happens here).
        w->step(kDt);
        mgr.update(kDt);
        bool broke = mgr.isBroken(id);

        // Count free debris chunks + verify at least one has non-trivial velocity.
        uint32_t chunkBodies = 0; bool anyMoving = false; bool anySpin = false;
        mgr.forEachActiveChunk([&](const ChunkView& v) {
            if (v.intact) return;
            ++chunkBodies;
            float lin[3], ang[3];
            w->getBodyLinearVelocity(v.body, lin);
            w->getBodyAngularVelocity(v.body, ang);
            if (std::sqrt(lin[0]*lin[0]+lin[1]*lin[1]+lin[2]*lin[2]) > 0.5f) anyMoving = true;
            if (std::sqrt(ang[0]*ang[0]+ang[1]*ang[1]+ang[2]*ang[2]) > 0.1f) anySpin = true;
        });
        dcheck(spawned && queued && notYet && broke && before == 1 &&
               chunkBodies == 8 && anyMoving && anySpin,
               "(a) hit>thresh -> parent removed + 8 chunks w/ split linear+angular vel");
        mgr.shutdown();
        w->shutdown();
    }

    // ---- (b) BELOW threshold -> no break ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        destructGround(w.get());
        DestructibleManager mgr;
        mgr.init(w.get());
        // Very HIGH thresholds so a gentle drop never breaks it.
        FractureAssetId asset = makeCrateAsset(mgr, /*impulse*/500.0f, /*vel*/200.0f);
        float xf[16]; xformAt(0.0f, 0.55f, 0.0f, xf);     // resting just above ground
        DestructibleId id = mgr.spawnDestructible(asset, xf);
        // Let it settle on the ground (gentle contact, well below thresholds).
        for (int i = 0; i < 120; ++i) { w->step(kDt); mgr.update(kDt); }
        bool stillIntact = !mgr.isBroken(id);
        bool noDebris = true;
        mgr.forEachActiveChunk([&](const ChunkView& v){ if (!v.intact) noDebris = false; });
        dcheck(stillIntact && noDebris, "(b) gentle contact below threshold -> NO break");
        mgr.shutdown();
        w->shutdown();
    }

    // ---- (c) contact-callback safety: breaks QUEUED + applied post-step, ZERO
    //          physics mutation inside the locked callback ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        destructGround(w.get());
        DestructibleManager mgr;
        mgr.init(w.get());
        // Low thresholds so a real contact triggers a break (drives the callback).
        FractureAssetId asset = makeCrateAsset(mgr, /*impulse*/1.0f, /*vel*/2.0f);
        float xf[16]; xformAt(0.0f, 3.0f, 0.0f, xf);
        DestructibleId id = mgr.spawnDestructible(asset, xf);
        // Give it a strong downward velocity so the ground impact is above threshold.
        float dn[3] = { 0.0f, -25.0f, 0.0f };
        w->setBodyLinearVelocity(mgr.intactBodyOf(id), dn);
        // Step until it impacts + breaks. After each step() Jolt has drained its
        // contact queue (our callback ran). The break must only be ENQUEUED there,
        // never applied — so sawMutationInCallback() must stay false the whole time.
        bool everBroke = false, safeThroughout = true;
        for (int i = 0; i < 240 && !everBroke; ++i) {
            w->step(kDt);
            if (mgr.sawMutationInCallback()) safeThroughout = false;  // tripped if a break ran in-callback
            mgr.update(kDt);                                          // fracture happens HERE (post-step)
            if (mgr.sawMutationInCallback()) safeThroughout = false;
            everBroke = mgr.isBroken(id);
        }
        bool safe = safeThroughout && !mgr.sawMutationInCallback();
        dcheck(everBroke && safe,
               "(c) contact break QUEUED + applied post-step; ZERO mutation in locked callback");
        mgr.shutdown();
        w->shutdown();
    }

    // ---- (d) chunk cap respected + chunks despawn/recycle (bounded, no leak) ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        destructGround(w.get());
        DestructibleManager mgr;
        DestructionTuning t;
        t.maxActiveChunks = 16;          // small cap so we exercise recycling
        t.chunkDespawnTime = 0.5f;       // short life so reaping is observable
        mgr.init(w.get(), t);
        FractureAssetId asset = makeCrateAsset(mgr, /*impulse*/1.0f, /*vel*/1.0f);

        // Spawn + break 10 crates (8 chunks each = 80 would-be debris) -> cap to 16.
        for (int n = 0; n < 10; ++n) {
            float xf[16]; xformAt((float)(n * 3), 2.0f, 0.0f, xf);
            DestructibleId id = mgr.spawnDestructible(asset, xf);
            float pt[3] = { (float)(n*3) - 2.0f, 2.0f, 0.0f };
            float dir[3] = { 1.0f, 0.0f, 0.0f };
            mgr.applyHit(pt, dir, 50.0f);
            w->step(kDt);
            mgr.update(kDt);
            (void)id;
        }
        // Count free debris bodies — must be capped.
        uint32_t debris = 0;
        mgr.forEachActiveChunk([&](const ChunkView& v){ if (!v.intact) ++debris; });
        bool capped = debris <= t.maxActiveChunks;

        // Now let time pass so the despawn timer reaps everything -> back to 0.
        for (int i = 0; i < 120; ++i) { w->step(kDt); mgr.update(kDt); }
        uint32_t after = 0;
        mgr.forEachActiveChunk([&](const ChunkView& v){ if (!v.intact) ++after; });
        bool reaped = (after == 0);
        dcheck(capped && reaped,
               "(d) chunk cap respected (<=16) + all debris despawned/recycled (no leak)");
        mgr.shutdown();
        w->shutdown();
    }

    x3::logInfo(std::string("[destruct-test] ") + std::to_string(d_pass) + " passed, " +
                std::to_string(d_fail) + " failed");
    return d_fail == 0;
}

} // namespace x3::phys
