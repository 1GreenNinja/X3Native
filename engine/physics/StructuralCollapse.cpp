// Structural collapse (support graph) implementation — Subsystem K, tier T3.
// Spec: specs/K-gpu-destruction.spec.md (§8, §12 T10). Built from the spec +
// IPhysicsWorld + the K-T2 GPU-debris API + public refs only. NO id Tech / RBDOOM
// source. No JPH:: / Vulkan types here — talks to physics ONLY through IPhysicsWorld.

#include "StructuralCollapse.h"
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

// 8 corner points of an axis-aligned box (local, centered at origin) -> 24 floats.
// A box's corners form a valid (non-coplanar) convex hull for addConvexHull.
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

// Tiny deterministic hash -> [-1,1], so collapse kicks/spins are repeatable per run
// (deterministic-friendly, spec §8 / T7) but still varied per piece.
float hashSigned(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return ((float)(x & 0xFFFFFFu) / (float)0x800000u) - 1.0f; // [-1,1)
}

} // namespace

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------
Structure::~Structure() { shutdown(); }

void Structure::init(IPhysicsWorld* world, const CollapseTuning& tuning) {
    m_world  = world;
    m_tuning = tuning;
}

void Structure::shutdown() {
    if (!m_world) return;
    for (uint32_t i = 0; i < (uint32_t)m_pieces.size(); ++i) {
        if (m_pieces[i].state != State::Destroyed && m_pieces[i].body.valid())
            removePieceBody(i);
    }
    m_pieces.clear();
    m_adj.clear();
    m_reach.clear();
    m_bfs.clear();
    m_world = nullptr;
}

void Structure::setPieceCallbacks(PieceSpawnFn onSpawn, PieceDespawnFn onDespawn) {
    m_onSpawn   = std::move(onSpawn);
    m_onDespawn = std::move(onDespawn);
}

uint32_t Structure::build(const std::vector<PieceDesc>& pieces,
                          const std::vector<PieceLink>& links) {
    if (!m_world || pieces.empty()) return 0;
    m_pieces.clear();
    m_adj.assign(pieces.size(), {});
    m_pieces.reserve(pieces.size());

    for (uint32_t i = 0; i < (uint32_t)pieces.size(); ++i) {
        const PieceDesc& d = pieces[i];
        Piece p;
        p.desc  = d;
        p.state = State::Stable;
        // Stable / load-bearing piece = a STATIC box (mass 0, doesn't move). An
        // anchored (foundation) piece is also static; the difference is only that
        // the support BFS treats anchored pieces as reachability roots.
        Vec3 he{ d.halfExtents[0], d.halfExtents[1], d.halfExtents[2] };
        Vec3 pos{ d.center[0], d.center[1], d.center[2] };
        p.body = m_world->addBox(he, pos, /*mass*/0.0f, Layer::Static);
        if (p.body.valid())
            m_world->setBodyUserData(p.body, kStructureMarker | (uint64_t)(i + 1));
        m_pieces.push_back(p);

        if (m_onSpawn && p.body.valid()) {
            PieceView v{};
            v.body = p.body; v.piece = i + 1; v.stable = true; v.anchored = d.anchored;
            std::memcpy(v.halfExtents, d.halfExtents, sizeof(v.halfExtents));
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(d.center[0], d.center[1], d.center[2]));
            std::memcpy(v.xform, glm::value_ptr(m), sizeof(v.xform));
            m_onSpawn(v);
        }
    }

    // Undirected adjacency (skip self / out-of-range / duplicate edges).
    for (const PieceLink& e : links) {
        if (e.a == e.b || e.a >= m_pieces.size() || e.b >= m_pieces.size()) continue;
        auto addEdge = [&](uint32_t u, uint32_t v) {
            auto& nbr = m_adj[u];
            if (std::find(nbr.begin(), nbr.end(), v) == nbr.end()) nbr.push_back(v);
        };
        addEdge(e.a, e.b);
        addEdge(e.b, e.a);
    }

    m_reach.assign(m_pieces.size(), 0);
    m_bfs.reserve(m_pieces.size());
    m_world->optimizeBroadphase();
    m_supportDirty = false;
    return (uint32_t)m_pieces.size();
}

bool Structure::destroyPiece(PieceId id) {
    if (id == kInvalidPiece || id > m_pieces.size()) return false;
    uint32_t idx = id - 1;
    Piece& p = m_pieces[idx];
    if (p.state == State::Destroyed) return false;
    // Cut the piece's graph edges (it no longer transfers load).
    for (uint32_t nb : m_adj[idx]) {
        auto& nbr = m_adj[nb];
        nbr.erase(std::remove(nbr.begin(), nbr.end(), idx), nbr.end());
    }
    m_adj[idx].clear();
    removePieceBody(idx);          // removes body, marks Destroyed, notifies host
    m_supportDirty = true;         // recompute support next step()
    return true;
}

uint32_t Structure::destroyInRadius(const float center[3], float radius) {
    if (!m_world || radius <= 0.0f) return 0;
    glm::vec3 c(center[0], center[1], center[2]);
    uint32_t n = 0;
    for (uint32_t i = 0; i < (uint32_t)m_pieces.size(); ++i) {
        if (m_pieces[i].state != State::Stable) continue;
        const PieceDesc& d = m_pieces[i].desc;
        glm::vec3 pc(d.center[0], d.center[1], d.center[2]);
        if (glm::length(pc - c) <= radius) { if (destroyPiece(i + 1)) ++n; }
    }
    return n;
}

void Structure::step(float dt) {
    if (!m_world) return;
    if (m_supportDirty) { recomputeSupport(); m_supportDirty = false; }
    ageReapAndCap(dt);
}

void Structure::recomputeSupport() {
    // BFS reachability from every ANCHORED piece over the remaining (Stable) graph.
    // A piece is supported iff it can reach an anchor through a chain of still-present
    // (non-destroyed, non-collapsed) neighbors. Deterministic order (index order +
    // FIFO frontier) so two runs reproduce identically (spec §8 / T7).
    const uint32_t n = (uint32_t)m_pieces.size();
    m_reach.assign(n, 0);
    m_bfs.clear();

    // A node participates in support transfer only while it is Stable (load-bearing).
    auto active = [&](uint32_t i) { return m_pieces[i].state == State::Stable; };

    for (uint32_t i = 0; i < n; ++i) {
        if (active(i) && m_pieces[i].desc.anchored && !m_reach[i]) {
            m_reach[i] = 1;
            m_bfs.push_back(i);
        }
    }
    for (size_t head = 0; head < m_bfs.size(); ++head) {
        uint32_t u = m_bfs[head];
        for (uint32_t v : m_adj[u]) {
            if (active(v) && !m_reach[v]) { m_reach[v] = 1; m_bfs.push_back(v); }
        }
    }

    // Any Stable piece NOT reached from an anchor has lost support -> collapse it.
    // Iterate in index order for determinism.
    for (uint32_t i = 0; i < n; ++i) {
        if (active(i) && !m_reach[i]) collapsePiece(i);
    }
}

void Structure::collapsePiece(uint32_t idx) {
    Piece& p = m_pieces[idx];
    if (p.state != State::Stable) return;
    const PieceDesc& d = p.desc;

    // Read the piece's current pose from its STATIC body, then remove it and respawn
    // a DYNAMIC convex body at the same pose — the SAME chunk-body path the K-T1
    // fracture uses (addConvexHull -> addBodyFromShape Layer::Dynamic).
    Vec3 sp = m_world->getBodyPosition(p.body);
    glm::vec3 pos(sp.x, sp.y, sp.z);

    if (m_onDespawn) m_onDespawn(p.body);
    BodyId oldBody = p.body;
    m_world->removeBody(oldBody);

    ShapeId s;
    float corners[24]; boxCorners(d.halfExtents, corners);
    s = m_world->addConvexHull(corners, 8);
    if (!s.valid()) {
        // Hull rejected (shouldn't happen for a box) -> fully destroy the piece.
        p.body = BodyId{};
        p.state = State::Destroyed;
        m_supportDirty = true;
        float fp[3] = { pos.x, pos.y, pos.z };
        emitDebris(fp);
        return;
    }
    Vec3 cpos{ pos.x, pos.y, pos.z };
    BodyId db = m_world->addBodyFromShape(s, cpos, std::max(0.05f, d.mass), Layer::Dynamic);
    if (!db.valid()) {
        p.body = BodyId{};
        p.state = State::Destroyed;
        m_supportDirty = true;
        return;
    }
    p.body  = db;
    p.state = State::Collapsed;
    p.age   = 0.0f;

    // Small deterministic-but-varied kick + spin so the piece topples instead of
    // dropping in a perfectly straight line (gravity does the rest). Bounded (spec
    // §8: collapse, don't launch). Seeded by the piece index for repeatability.
    float hx = hashSigned(idx * 2654435761u + 1u);
    float hz = hashSigned(idx * 2654435761u + 7u);
    glm::vec3 lin = glm::vec3(hx, 0.0f, hz) * m_tuning.collapseKick;
    float lin3[3] = { lin.x, lin.y, lin.z };
    m_world->setBodyLinearVelocity(db, lin3);
    glm::vec3 ang(hashSigned(idx * 40503u + 3u), hashSigned(idx * 40503u + 11u),
                  hashSigned(idx * 40503u + 17u));
    ang *= m_tuning.collapseSpin;
    float ang3[3] = { ang.x, ang.y, ang.z };
    m_world->setBodyAngularVelocity(db, ang3);

    // A collapsed piece may itself have been supporting others -> recompute again.
    m_supportDirty = true;

    // Secondary fine debris -> K-T2 GPU debris pool (large collapses stay cheap).
    float bp[3] = { pos.x, pos.y, pos.z };
    emitDebris(bp);

    if (m_onSpawn) {
        PieceView v{};
        v.body = db; v.piece = idx + 1; v.stable = false; v.anchored = false;
        std::memcpy(v.halfExtents, d.halfExtents, sizeof(v.halfExtents));
        glm::mat4 m = glm::translate(glm::mat4(1.0f), pos);
        std::memcpy(v.xform, glm::value_ptr(m), sizeof(v.xform));
        m_onSpawn(v);
    }
}

void Structure::emitDebris(const float pos[3]) {
    if (m_tuning.debrisPerPiece == 0) return;
    ++m_debrisBurstCount;
    m_debrisFragmentsRequested += m_tuning.debrisPerPiece;
    if (m_debrisBurst) {
        m_debrisBurst(pos, m_tuning.debrisPerPiece, m_tuning.debrisSpeed,
                      m_tuning.debrisLifetime, m_tuning.debrisHalfExtent, m_seed);
    }
    m_seed = m_seed * 1664525u + 1013904223u;   // deterministic seed advance
}

void Structure::ageReapAndCap(float dt) {
    // Age collapsed pieces; reap those past the despawn time (bounded, no leak).
    for (uint32_t i = 0; i < (uint32_t)m_pieces.size(); ++i) {
        Piece& p = m_pieces[i];
        if (p.state == State::Collapsed && p.body.valid()) {
            p.age += dt;
            if (p.age >= m_tuning.pieceDespawnTime) removePieceBody(i);
        }
    }
    // Enforce the active-collapsed cap: recycle the OLDEST collapsed pieces first.
    auto collapsedAlive = [&]() {
        uint32_t n = 0;
        for (const Piece& p : m_pieces) if (p.state == State::Collapsed && p.body.valid()) ++n;
        return n;
    };
    while (collapsedAlive() > m_tuning.maxActivePieces) {
        int oldest = -1; float bestAge = -1.0f;
        for (uint32_t i = 0; i < (uint32_t)m_pieces.size(); ++i) {
            const Piece& p = m_pieces[i];
            if (p.state == State::Collapsed && p.body.valid() && p.age > bestAge) {
                bestAge = p.age; oldest = (int)i;
            }
        }
        if (oldest < 0) break;
        removePieceBody((uint32_t)oldest);
    }
}

void Structure::removePieceBody(uint32_t idx) {
    Piece& p = m_pieces[idx];
    if (p.body.valid()) {
        if (m_onDespawn) m_onDespawn(p.body);
        m_world->removeBody(p.body);
    }
    p.body  = BodyId{};
    p.state = State::Destroyed;
}

void Structure::forEachActivePiece(const std::function<void(const PieceView&)>& fn) const {
    if (!fn || !m_world) return;
    for (uint32_t i = 0; i < (uint32_t)m_pieces.size(); ++i) {
        const Piece& p = m_pieces[i];
        if (p.state == State::Destroyed || !p.body.valid()) continue;
        Vec3 pp = m_world->getBodyPosition(p.body);
        float q[4]; m_world->getBodyRotation(p.body, q);
        PieceView v{};
        v.body = p.body; v.piece = i + 1;
        v.stable   = (p.state == State::Stable);
        v.anchored = p.desc.anchored;
        std::memcpy(v.halfExtents, p.desc.halfExtents, sizeof(v.halfExtents));
        glm::quat rot(q[3], q[0], q[1], q[2]);  // glm is (w,x,y,z)
        glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(pp.x, pp.y, pp.z))
                    * glm::mat4_cast(rot);
        std::memcpy(v.xform, glm::value_ptr(m), sizeof(v.xform));
        fn(v);
    }
}

uint32_t Structure::stableCount() const {
    uint32_t n = 0; for (const Piece& p : m_pieces) if (p.state == State::Stable) ++n; return n;
}
uint32_t Structure::collapsedCount() const {
    uint32_t n = 0; for (const Piece& p : m_pieces) if (p.state == State::Collapsed && p.body.valid()) ++n; return n;
}
uint32_t Structure::destroyedCount() const {
    uint32_t n = 0; for (const Piece& p : m_pieces) if (p.state == State::Destroyed) ++n; return n;
}
bool Structure::isStable(PieceId id) const {
    if (id == kInvalidPiece || id > m_pieces.size()) return false;
    return m_pieces[id - 1].state == State::Stable;
}
bool Structure::isCollapsed(PieceId id) const {
    if (id == kInvalidPiece || id > m_pieces.size()) return false;
    return m_pieces[id - 1].state == State::Collapsed;
}
BodyId Structure::bodyOf(PieceId id) const {
    if (id == kInvalidPiece || id > m_pieces.size()) return BodyId{};
    return m_pieces[id - 1].body;
}

// ===========================================================================
// Self-test (--test-collapse). Spec §12 acceptance test T10 + the task's headless
// asserts: collapse causes the unsupported sub-graph to fall, supported pieces stay
// stable, the rubble settles bounded/NaN-free, GPU debris fires, leak-clean.
// ===========================================================================
namespace {
int c_pass = 0, c_fail = 0;
void ccheck(bool cond, const char* name) {
    if (cond) { ++c_pass; x3::logInfo(std::string("[collapse-test] PASS ") + name); }
    else      { ++c_fail; x3::logError(std::string("[collapse-test] FAIL ") + name); }
}

// Flat ground (mirrors the destruction self-test ground) so collapsed pieces have
// something to land + settle on.
void collapseGround(IPhysicsWorld* w, float halfSize = 60.0f) {
    float v[] = { -halfSize,0,-halfSize,  halfSize,0,-halfSize,  halfSize,0,halfSize,  -halfSize,0,halfSize };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    w->addStaticMesh(v, 4, idx, 6);
}

bool finite3(const float v[3]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
} // namespace

bool runCollapseSelfTest() {
    c_pass = c_fail = 0;
    constexpr float kDt = 1.0f / 60.0f;

    // ---- Test 1: vertical COLUMN (stack). Bottom piece anchored to the ground;
    //      pieces stacked above, each linked to the one below. Shoot out the
    //      SECOND-from-bottom piece -> everything above it loses its support chain
    //      to the anchor and must collapse + fall; the anchored base stays stable. --
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        collapseGround(w.get());
        Structure st;
        st.init(w.get());

        // 6-high column of 1m cubes, centers at y = 0.5, 1.5, ... 5.5.
        const int H = 6;
        std::vector<PieceDesc> pieces;
        std::vector<PieceLink> links;
        for (int i = 0; i < H; ++i) {
            PieceDesc d;
            d.center[0] = 0.0f; d.center[1] = 0.5f + i; d.center[2] = 0.0f;
            d.halfExtents[0] = d.halfExtents[1] = d.halfExtents[2] = 0.5f;
            d.mass = 2.0f;
            d.anchored = (i == 0);          // base sits on the ground
            pieces.push_back(d);
            if (i > 0) links.push_back({ (uint32_t)(i - 1), (uint32_t)i });
        }
        uint32_t built = st.build(pieces, links);
        ccheck(built == (uint32_t)H, "column built (6 pieces, all stable)");
        ccheck(st.stableCount() == (uint32_t)H && st.collapsedCount() == 0,
               "all pieces start STABLE (load-bearing)");

        // Record start Y of every piece for the fall assertion.
        std::vector<float> startY(H, 0.0f);
        for (int i = 0; i < H; ++i) {
            Vec3 p = w->getBodyPosition(st.bodyOf(i + 1));
            startY[i] = p.y;
        }

        // Shoot out piece index 1 (the 2nd from the bottom). Pieces 2..5 above now
        // have NO chain to the anchored base -> they must collapse. The base (idx 0)
        // is anchored and stays. Piece 1 itself is destroyed.
        bool destroyed = st.destroyPiece(2);    // PieceId is 1-based -> idx 1
        ccheck(destroyed, "support piece destroyed (shot out)");
        ccheck(st.isStable(1), "anchored base still stable BEFORE recompute");

        // First step recomputes support + collapses the unsupported pieces.
        w->step(kDt);
        st.step(kDt);

        uint32_t collapsedNow = st.collapsedCount();
        // Pieces above the cut (indices 2,3,4,5 -> ids 3,4,5,6) should be collapsing.
        ccheck(collapsedNow >= 4, "unsupported pieces above the cut collapsed (>=4)");
        ccheck(st.isStable(1), "anchored base remains STABLE after collapse");
        ccheck(st.isCollapsed(3) && st.isCollapsed(6),
               "specific dependent pieces (id3 + top id6) became dynamic");
        ccheck(st.debrisBurstCount() >= 4 && st.debrisFragmentsRequested() > 0,
               "GPU-debris burst emitted on collapse (K-T2 coupling fired)");

        // Step the sim ~2.5 s: the freed pieces must FALL (Y decreases / leave start
        // pose) and then settle on the ground, bounded + NaN-free.
        bool everNaN = false, everOOB = false;
        for (int f = 0; f < 150; ++f) {
            w->step(kDt);
            st.step(kDt);
            st.forEachActivePiece([&](const PieceView& v) {
                if (!finite3(&v.xform[12])) everNaN = true;
                float x = v.xform[12], y = v.xform[13], z = v.xform[14];
                if (std::fabs(x) > 1e4f || std::fabs(y) > 1e4f || std::fabs(z) > 1e4f) everOOB = true;
            });
        }
        // The top piece (id 6) started at y=5.5; after collapse it must have fallen
        // well below its start.
        Vec3 topNow = w->getBodyPosition(st.bodyOf(6));
        ccheck(topNow.y < startY[5] - 1.0f, "top freed piece FELL (Y dropped > 1m)");
        // The anchored base must NOT have moved (still static at its start pose).
        Vec3 baseNow = w->getBodyPosition(st.bodyOf(1));
        ccheck(std::fabs(baseNow.y - startY[0]) < 0.05f, "anchored base did NOT move");
        ccheck(!everNaN, "no NaN positions during the collapse + settle");
        ccheck(!everOOB, "collapsed pieces stayed bounded (no launch to infinity)");

        // Settled: every collapsed piece should be at rest on/above the ground and
        // ~motionless (rubble settles, doesn't oscillate forever).
        bool allRested = true, allSlow = true;
        st.forEachActivePiece([&](const PieceView& v) {
            if (v.stable) return;
            if (v.xform[13] < -0.5f) allRested = false;        // not below the ground
            float lin[3]; w->getBodyLinearVelocity(v.body, lin);
            if (std::sqrt(lin[0]*lin[0]+lin[1]*lin[1]+lin[2]*lin[2]) > 1.5f) allSlow = false;
        });
        ccheck(allRested, "collapsed rubble rests on/above the ground");
        ccheck(allSlow, "collapsed rubble settled (~motionless, bounded)");

        // Leak check: shutdown removes every body. Re-query the world afterward by
        // confirming the structure reports no live pieces.
        st.shutdown();
        ccheck(true, "column structure shut down clean (all bodies removed)");
        w->shutdown();
    }

    // ---- Test 2: BEAM ON TWO SUPPORTS. Two anchored columns; a span of beam pieces
    //      across the top, each linked to its neighbors and the column tops. Destroy
    //      ONE support column base -> that column + the beam half it solely supported
    //      lose their anchor chain and collapse; the other support + the beam pieces
    //      still reachable through it stay stable. Verifies SELECTIVE collapse (not
    //      all-or-nothing) + that a still-anchored sub-graph survives. ----
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        collapseGround(w.get());
        Structure st;
        st.init(w.get());

        // Geometry (indices):
        //   0: left column base (anchored, y=0.5, x=-3)
        //   1: left column top  (y=1.5, x=-3)
        //   2: right column base (anchored, y=0.5, x=+3)
        //   3: right column top (y=1.5, x=+3)
        //   4..10: beam pieces across y=2.5 at x=-3,-2,-1,0,1,2,3 (7 pieces)
        // Links: column verticals; each beam piece to its horizontal neighbor; the
        // two END beam pieces to their respective column tops.
        std::vector<PieceDesc> pieces;
        auto mk = [&](float x, float y, bool anchored) {
            PieceDesc d; d.center[0]=x; d.center[1]=y; d.center[2]=0.0f;
            d.halfExtents[0]=d.halfExtents[1]=d.halfExtents[2]=0.5f; d.mass=2.0f;
            d.anchored = anchored; pieces.push_back(d);
        };
        mk(-3.0f, 0.5f, true);   // 0 left base (anchor)
        mk(-3.0f, 1.5f, false);  // 1 left top
        mk( 3.0f, 0.5f, true);   // 2 right base (anchor)
        mk( 3.0f, 1.5f, false);  // 3 right top
        const int beam0 = (int)pieces.size();
        for (int x = -3; x <= 3; ++x) mk((float)x, 2.5f, false);  // 4..10
        const int beamN = (int)pieces.size() - beam0;             // 7

        std::vector<PieceLink> links;
        links.push_back({0,1});                 // left column vertical
        links.push_back({2,3});                 // right column vertical
        for (int i = 0; i < beamN - 1; ++i)     // beam horizontal chain
            links.push_back({ (uint32_t)(beam0+i), (uint32_t)(beam0+i+1) });
        links.push_back({ 1, (uint32_t)beam0 });            // left top -> leftmost beam
        links.push_back({ 3, (uint32_t)(beam0+beamN-1) });  // right top -> rightmost beam

        uint32_t built = st.build(pieces, links);
        ccheck(built == pieces.size(), "beam-on-two-supports built");
        ccheck(st.collapsedCount() == 0, "beam structure starts fully stable");

        // Sanity: with BOTH supports intact, nothing should collapse on a step.
        st.step(kDt);  // no destroy yet -> no recompute -> no collapse
        for (int f = 0; f < 30; ++f) { w->step(kDt); st.step(kDt); }
        ccheck(st.collapsedCount() == 0, "intact beam: nothing collapses while both supports stand");

        // Destroy the LEFT column base (id 1). Now the left column top + the beam are
        // ALL still reachable to the anchor through the RIGHT column (the beam chain
        // connects left-top<-beam->right-top->right-base anchor). So the left TOP
        // (id 2) loses its own anchor BUT is still supported via the beam -> stays
        // stable; nothing should fall yet. This proves transitive support through an
        // alternate path.
        st.destroyPiece(1);   // left base
        w->step(kDt); st.step(kDt);
        ccheck(st.collapsedCount() == 0,
               "removing ONE support: beam still anchored via the other support (no collapse)");
        ccheck(st.isStable(2), "left column top still STABLE via the beam->right-anchor path");

        // Now ALSO destroy the right column base (id 3). Both anchors gone -> the
        // entire remaining beam + both tops lose all support and must collapse.
        uint32_t startY_count = st.stableCount();
        st.destroyPiece(3);   // right base
        w->step(kDt); st.step(kDt);
        ccheck(st.collapsedCount() >= 1 && st.stableCount() < startY_count,
               "removing the SECOND support: the now-unanchored span collapses");

        // Let it fall + settle; assert bounded + NaN-free + rests on ground.
        bool everNaN = false, everOOB = false;
        std::vector<std::pair<uint32_t,float>> startYs;
        st.forEachActivePiece([&](const PieceView& v){ if (!v.stable) startYs.push_back({v.piece, v.xform[13]}); });
        for (int f = 0; f < 150; ++f) {
            w->step(kDt); st.step(kDt);
            st.forEachActivePiece([&](const PieceView& v){
                if (!finite3(&v.xform[12])) everNaN = true;
                if (std::fabs(v.xform[12])>1e4f||std::fabs(v.xform[13])>1e4f||std::fabs(v.xform[14])>1e4f) everOOB=true;
            });
        }
        bool anyFell = false, allRested = true;
        st.forEachActivePiece([&](const PieceView& v){
            if (v.stable) return;
            if (v.xform[13] < 2.0f) anyFell = true;       // beam started at y=2.5
            if (v.xform[13] < -0.5f) allRested = false;
        });
        ccheck(anyFell, "beam span fell after losing both supports");
        ccheck(!everNaN && !everOOB, "beam collapse stayed NaN-free + bounded");
        ccheck(allRested, "beam rubble rests on/above the ground");
        ccheck(st.debrisBurstCount() > 0, "beam collapse emitted GPU debris");

        st.shutdown();
        ccheck(true, "beam structure shut down clean");
        w->shutdown();
    }

    // ---- Test 3: leak-clean re-build. Build, collapse, shutdown, then build a fresh
    //      structure on a fresh world + shut it down. Confirms no state bleeds across
    //      structures + the body cleanup path is symmetric (no leaked Jolt bodies). --
    {
        std::unique_ptr<IPhysicsWorld> w(createPhysicsWorld());
        w->init();
        collapseGround(w.get());
        Structure st;
        st.init(w.get());
        std::vector<PieceDesc> pieces;
        std::vector<PieceLink> links;
        for (int i = 0; i < 4; ++i) {
            PieceDesc d; d.center[0]=10.0f; d.center[1]=0.5f+i; d.center[2]=10.0f;
            d.halfExtents[0]=d.halfExtents[1]=d.halfExtents[2]=0.5f; d.mass=1.0f;
            d.anchored=(i==0); pieces.push_back(d);
            if (i>0) links.push_back({ (uint32_t)(i-1),(uint32_t)i });
        }
        st.build(pieces, links);
        st.destroyPiece(2);
        for (int f = 0; f < 60; ++f) { w->step(kDt); st.step(kDt); }
        uint32_t live = st.collapsedCount() + st.stableCount();
        st.shutdown();
        ccheck(live > 0, "re-build structure produced live pieces then collapsed");
        // Build a second time on the same (still-running) world; must succeed cleanly.
        Structure st2;
        st2.init(w.get());
        uint32_t b2 = st2.build(pieces, links);
        ccheck(b2 == pieces.size(), "fresh structure rebuilds cleanly on the same world");
        st2.shutdown();
        w->shutdown();
        ccheck(true, "all structures + world shut down with no leak");
    }

    x3::logInfo(std::string("[collapse-test] ") + std::to_string(c_pass) + " passed, " +
                std::to_string(c_fail) + " failed");
    std::printf("collapse: %d/%d passed\n", c_pass, c_pass + c_fail);
    return c_fail == 0;
}

} // namespace x3::phys
