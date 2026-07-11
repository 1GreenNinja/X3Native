// CROWDS implementation — see app/crowd.h for the design stance.
// Game/slice code only — engine/ stays pure.

#include "crowd.h"
#include "crowd_skin.h"   // the skinned-citizen section of --test-crowd
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

const char* crowdStateName(CrowdState s) {
    switch (s) {
        case CrowdState::Idle:     return "Idle";
        case CrowdState::Wander:   return "Wander";
        case CrowdState::Scatter:  return "Scatter";
        case CrowdState::Cower:    return "Cower";
        case CrowdState::Converse: return "Converse";
        case CrowdState::Work:     return "Work";
        case CrowdState::Play:     return "Play";
    }
    return "?";
}

namespace {

// Civilian humanoid: torso/head/legs/arms blocks (no rifle — these are guests).
x3::prims::PrimMesh makeCivilianMesh() {
    x3::prims::PrimMesh m;
    auto box = [&](float hx, float hy, float hz, float cx, float cy, float cz) {
        x3::prims::PrimMesh b = x3::prims::makeBox(hx, hy, hz, cx, cy, cz);
        const uint32_t base = (uint32_t)m.verts.size();
        m.verts.insert(m.verts.end(), b.verts.begin(), b.verts.end());
        for (uint32_t i : b.index) m.index.push_back(base + i);
    };
    box(0.21f, 0.33f, 0.12f,  0.00f, 1.04f, 0.00f);   // torso
    box(0.11f, 0.12f, 0.11f,  0.00f, 1.55f, 0.00f);   // head
    box(0.085f, 0.39f, 0.085f, -0.115f, 0.39f, 0.00f); // legs
    box(0.085f, 0.39f, 0.085f,  0.115f, 0.39f, 0.00f);
    box(0.065f, 0.30f, 0.065f, -0.285f, 1.02f, 0.00f); // arms
    box(0.065f, 0.30f, 0.065f,  0.285f, 1.02f, 0.00f);
    return m;
}

// Club/civilian outfit palette (linear RGB).
constexpr float kPalette[][3] = {
    { 0.85f, 0.20f, 0.55f },   // magenta
    { 0.20f, 0.75f, 0.85f },   // cyan
    { 0.80f, 0.70f, 0.25f },   // gold
    { 0.45f, 0.30f, 0.80f },   // violet
    { 0.25f, 0.70f, 0.35f },   // green
    { 0.80f, 0.35f, 0.25f },   // ember
    { 0.70f, 0.70f, 0.75f },   // silver
    { 0.30f, 0.40f, 0.85f },   // blue
};
constexpr uint32_t kPaletteN = (uint32_t)(sizeof(kPalette) / sizeof(kPalette[0]));

inline float dist2XZ(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

// CARRY task micro-phases (CrowdAgent::workPhase for Kind::Carry).
// The pickup target is THE CRATE (wherever it lies — normally at A, or where a
// scattering carrier dropped it), so the calm-return read is "walk back to the
// dropped crate and get on with it".
constexpr uint32_t kCarryToCrate = 0;   // walk to the crate
constexpr uint32_t kCarryHoist   = 1;   // dwell: hoisting it up
constexpr uint32_t kCarryToB     = 2;   // carry (leaning) to the drop-off
constexpr uint32_t kCarryDrop    = 3;   // dwell: setting it down
constexpr uint32_t kCarryReturn  = 4;   // walk back empty to A (crate restocks)

// SWEEP: workPhase 0 = pace to A, 1 = pace to B.

// PLAY knot ring radius + kickabout pass speed.
constexpr float kKnotRadius = 2.3f;
constexpr float kPassSpeed  = 4.5f;

} // namespace

uint32_t CrowdSystem::rng() {
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return m_rngState;
}

float CrowdSystem::frand() { return (float)(rng() % 10000) * 0.0001f; }

void CrowdSystem::clampToRegion(float& x, float& z) const {
    // Rect clamp (hallways / street bands) when authored.
    if (m_cfg.halfX > 0.0f && m_cfg.halfZ > 0.0f) {
        x = std::clamp(x, m_cfg.centerX - m_cfg.halfX, m_cfg.centerX + m_cfg.halfX);
        z = std::clamp(z, m_cfg.centerZ - m_cfg.halfZ, m_cfg.centerZ + m_cfg.halfZ);
        return;
    }
    const float dx = x - m_cfg.centerX, dz = z - m_cfg.centerZ;
    const float d2 = dx * dx + dz * dz;
    const float r  = m_cfg.radius;
    if (d2 <= r * r || d2 < 1e-6f) return;
    const float d = std::sqrt(d2);
    x = m_cfg.centerX + dx / d * r;
    z = m_cfg.centerZ + dz / d * r;
}

void CrowdSystem::writeTransform(CrowdAgent& a, Scene& scene, float bob, float crouch,
                                 float lean) {
    if (a.entity == kNoLink) return;
    Entity& e = scene.get(a.entity);
    const float s  = m_cfg.scale;
    const float sy = s * crouch;
    const float cy = std::cos(a.yaw), sn = std::sin(a.yaw);
    const float ca = std::cos(lean), sa = std::sin(lean);
    // R = Ry(yaw) * Rx(-lean): positive lean pitches the torso toward the local
    // -Z facing (the carry/tend lean-in). lean == 0 degenerates to the old
    // yaw-only matrix exactly.
    float* t = e.transform;
    t[0] = cy * s;        t[1] = 0;       t[2]  = -sn * s;       t[3]  = 0;
    t[4] = -sn * sa * sy; t[5] = ca * sy; t[6]  = -cy * sa * sy; t[7]  = 0;
    t[8] = sn * ca * s;   t[9] = sa * s;  t[10] = cy * ca * s;   t[11] = 0;
    t[12] = a.pos.x; t[13] = a.pos.y + bob; t[14] = a.pos.z; t[15] = 1;
}

void CrowdSystem::writePropTransform(uint32_t entity, Scene& scene,
                                     float x, float y, float z, float yaw, float s) {
    if (entity == kNoLink) return;
    Entity& e = scene.get(entity);
    const float cy = std::cos(yaw), sn = std::sin(yaw);
    float* t = e.transform;
    t[0] = cy * s; t[1] = 0; t[2]  = -sn * s; t[3]  = 0;
    t[4] = 0;      t[5] = s; t[6]  = 0;       t[7]  = 0;
    t[8] = sn * s; t[9] = 0; t[10] = cy * s;  t[11] = 0;
    t[12] = x; t[13] = y; t[14] = z; t[15] = 1;
}

void CrowdSystem::faceToward(CrowdAgent& a, float x, float z, float dt, float rate) {
    const float dx = x - a.pos.x, dz = z - a.pos.z;
    if (dx * dx + dz * dz < 1e-4f) return;
    const float want = std::atan2(-dx, -dz);   // -Z forward convention
    float d = want - a.yaw;
    while (d >  3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    const float mx = rate * dt;
    a.yaw += std::clamp(d, -mx, mx);
}

void CrowdSystem::build(const CrowdConfig& cfg, Scene& scene,
                        x3::rhi::IRenderDevice& device) {
    if (m_built) return;
    m_cfg = cfg;
    if (m_cfg.count == 0) m_cfg.count = 1;
    if (m_cfg.radius < 2.0f) m_cfg.radius = 2.0f;

    // Resolve hangout points: authored, else a ring of 4 around the center.
    if (m_cfg.points.size() >= 2) {
        m_points = m_cfg.points;
    } else {
        const float r = m_cfg.radius * 0.5f;
        m_points = { m_cfg.centerX + r, m_cfg.centerZ,
                     m_cfg.centerX - r, m_cfg.centerZ,
                     m_cfg.centerX,     m_cfg.centerZ + r,
                     m_cfg.centerX,     m_cfg.centerZ - r };
    }

    x3::prims::PrimMesh pm = makeCivilianMesh();
    m_mesh = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                               pm.index.data(), (uint32_t)pm.index.size());

    // ---- Role plan: one Worker per work point, `players` Gamers per play
    // spot, the rest Civilians (all clamped to the agent count). ----
    const uint32_t nWork = std::min((uint32_t)m_cfg.work.size(), m_cfg.count);
    // Prop meshes (created only when a deployment needs them).
    bool anyCarry = false;
    for (const CrowdWorkPoint& wp : m_cfg.work)
        if (wp.kind == CrowdWorkPoint::Kind::Carry) anyCarry = true;
    bool anyBall = false;
    for (const CrowdPlaySpot& ps : m_cfg.play) if (ps.ball) anyBall = true;
    if (anyCarry) {
        x3::prims::PrimMesh cm = x3::prims::makeBox(0.24f, 0.19f, 0.24f, 0.0f, 0.19f, 0.0f);
        m_crateMesh = device.createMesh(cm.verts.data(), (uint32_t)cm.verts.size(),
                                        cm.index.data(), (uint32_t)cm.index.size());
    }
    if (anyBall) {
        x3::prims::PrimMesh bm = x3::prims::makeBox(0.11f, 0.11f, 0.11f, 0.0f, 0.11f, 0.0f);
        m_ballMesh = device.createMesh(bm.verts.data(), (uint32_t)bm.verts.size(),
                                       bm.index.data(), (uint32_t)bm.index.size());
    }

    // Crates: one per Carry work point, seeded at its A.
    m_crateForWork.assign(m_cfg.work.size(), kNoLink);
    for (uint32_t w = 0; w < (uint32_t)m_cfg.work.size(); ++w) {
        if (m_cfg.work[w].kind != CrowdWorkPoint::Kind::Carry) continue;
        CrowdProp c;
        c.pos = { m_cfg.work[w].ax, m_cfg.groundY, m_cfg.work[w].az };
        Entity e;
        e.mesh = m_crateMesh;
        e.baseColor[0] = 0.78f; e.baseColor[1] = 0.55f; e.baseColor[2] = 0.24f;   // amber cargo
        e.baseColor[3] = 1.0f;
        e.roomId = m_cfg.roomId;
        c.entity = scene.add(e);
        m_crateForWork[w] = (uint32_t)m_crates.size();
        m_crates.push_back(c);
        writePropTransform(c.entity, scene, c.pos.x, c.pos.y, c.pos.z, 0.0f, 1.0f);
    }
    // Balls: one per ball play spot (index parity with cfg.play).
    for (const CrowdPlaySpot& ps : m_cfg.play) {
        CrowdProp b;
        b.pos = { ps.cx, m_cfg.groundY, ps.cz };
        if (ps.ball) {
            Entity e;
            e.mesh = m_ballMesh;
            e.baseColor[0] = 0.95f; e.baseColor[1] = 0.45f; e.baseColor[2] = 0.15f; // hi-viz orange
            e.baseColor[3] = 1.0f;
            e.emissive[0] = 0.95f; e.emissive[1] = 0.45f; e.emissive[2] = 0.15f;
            e.emissive[3] = 0.35f;   // reads in dusk streets
            e.roomId = m_cfg.roomId;
            b.entity = scene.add(e);
            writePropTransform(b.entity, scene, b.pos.x, b.pos.y, b.pos.z, 0.0f, 1.0f);
        }
        m_balls.push_back(b);
    }

    const uint32_t npts = (uint32_t)(m_points.size() / 2);
    uint32_t nextWork = 0;
    uint32_t spotIdx = 0, spotFill = 0;
    for (uint32_t k = 0; k < m_cfg.count; ++k) {
        CrowdAgent a;
        // ---- Assign the role + seed position ----
        if (nextWork < nWork) {
            a.role = CrowdRole::Worker;
            a.workIdx = nextWork++;
            const CrowdWorkPoint& wp = m_cfg.work[a.workIdx];
            a.pos.x = wp.ax + (frand() - 0.5f);
            a.pos.z = wp.az + (frand() - 0.5f);
            a.workPhase = 0;
        } else if (spotIdx < (uint32_t)m_cfg.play.size()) {
            const CrowdPlaySpot& ps = m_cfg.play[spotIdx];
            const uint32_t want = ps.ball ? std::max(2u, ps.players) : 2u;
            a.role = CrowdRole::Gamer;
            a.playIdx = spotIdx;
            a.slot = spotFill;
            const float ang = 6.2831853f * (float)spotFill / (float)want;
            a.pos.x = ps.cx + std::cos(ang) * (ps.ball ? kKnotRadius : 0.55f);
            a.pos.z = ps.cz + std::sin(ang) * (ps.ball ? kKnotRadius : 0.55f);
            if (++spotFill >= want) { ++spotIdx; spotFill = 0; }
        } else {
            a.role = CrowdRole::Civilian;
            // Seed in a loose knot around one of the points.
            const uint32_t pi = (npts > 0) ? (k % npts) : 0;
            const float px = (npts > 0) ? m_points[pi * 2 + 0] : m_cfg.centerX;
            const float pz = (npts > 0) ? m_points[pi * 2 + 1] : m_cfg.centerZ;
            const float ang = (float)(rng() % 6283) * 0.001f;
            const float rad = 0.6f + (float)(rng() % 1000) * 0.0016f;   // 0.6..2.2 m
            a.pos.x = px + std::cos(ang) * rad;
            a.pos.z = pz + std::sin(ang) * rad;
        }
        clampToRegion(a.pos.x, a.pos.z);
        a.pos.y = m_cfg.groundY;
        a.target = a.pos;
        a.yaw   = (float)(rng() % 6283) * 0.001f;
        a.phase = (float)(rng() % 6283) * 0.001f;
        a.timer = m_cfg.dwellMin
                + (float)(rng() % 1000) * 0.001f * (m_cfg.dwellMax - m_cfg.dwellMin);
        a.chatCooldown = 1.0f + frand() * 6.0f;   // stagger the first pair-ups
        if (a.role == CrowdRole::Worker) { a.state = CrowdState::Work; a.timer = 0.0f; }
        if (a.role == CrowdRole::Gamer)  { a.state = CrowdState::Play; a.timer = frand() * 2.0f; }

        const float* col = kPalette[k % kPaletteN];
        const float jit = 0.85f + (float)(rng() % 1000) * 0.0003f;
        Entity e;
        e.mesh = m_mesh;
        e.baseColor[0] = col[0] * jit;
        e.baseColor[1] = col[1] * jit;
        e.baseColor[2] = col[2] * jit;
        e.baseColor[3] = 1.0f;
        if (m_cfg.emissive > 0.0f) {
            e.emissive[0] = col[0]; e.emissive[1] = col[1]; e.emissive[2] = col[2];
            e.emissive[3] = m_cfg.emissive;
        }
        e.roomId = m_cfg.roomId;
        a.entity = scene.add(e);
        writeTransform(a, scene, 0.0f, 1.0f, 0.0f);
        m_agents.push_back(a);
    }
    m_built = true;
    x3::logInfo("crowd: built " + std::to_string(m_agents.size()) + " NPCs (" +
                std::to_string(nWork) + " workers, " +
                std::to_string(countInState(CrowdState::Play)) + " players, " +
                std::to_string(m_crates.size()) + " crates, " +
                std::to_string(m_balls.size()) + " play spots)");
}

void CrowdSystem::abandon() {
    // REGION SAFETY: the region ownership ledger owns every entity/mesh this
    // system created (they were built inside the realize's capture window) —
    // forget them WITHOUT touching the Scene or the device, and go unbuilt so
    // the next region realize can build() fresh.
    m_agents.clear();
    m_points.clear();
    m_crates.clear();
    m_crateForWork.clear();
    m_balls.clear();
    m_mesh = {};
    m_crateMesh = {};
    m_ballMesh = {};
    m_calmTimer = 0.0f;
    m_time = 0.0f;
    m_built = false;
}

uint32_t CrowdSystem::countInState(CrowdState s) const {
    uint32_t n = 0;
    for (const CrowdAgent& a : m_agents) if (a.state == s) ++n;
    return n;
}

void CrowdSystem::onViolence(const x3::phys::Vec3& pos) {
    if (!m_built) return;
    const float r2 = m_cfg.scatterRadius * m_cfg.scatterRadius;
    // A shot with NO agent in earshot is ignored — a distant indoor gunfight
    // never pauses a street kickabout on the other side of the map.
    bool anyNear = false;
    for (const CrowdAgent& a : m_agents)
        if (dist2XZ(a.pos, pos) <= r2) { anyNear = true; break; }
    if (!anyNear) return;
    m_calmTimer = m_cfg.calmTime;
    for (CrowdAgent& a : m_agents) {
        if (dist2XZ(a.pos, pos) > r2) continue;
        // Drop what you're doing: the conversation dissolves, a carried crate
        // lands where the carrier stands.
        if (a.partner != kNoLink) { a.partner = kNoLink; a.chatCooldown = 6.0f + frand() * 8.0f; }
        if (a.role == CrowdRole::Worker && a.workIdx != kNoLink &&
            a.workIdx < (uint32_t)m_crateForWork.size() &&
            m_crateForWork[a.workIdx] != kNoLink) {
            CrowdProp& c = m_crates[m_crateForWork[a.workIdx]];
            if (c.carried) {
                c.carried = false;
                c.pos = { a.pos.x, m_cfg.groundY, a.pos.z };
            }
        }
        if (a.role == CrowdRole::Worker) a.workPhase = kCarryToCrate;
        float ax = a.pos.x - pos.x, az = a.pos.z - pos.z;
        const float len = std::sqrt(ax * ax + az * az);
        if (len > 0.01f) { ax /= len; az /= len; }
        else { const float t = (float)(rng() % 6283) * 0.001f; ax = std::cos(t); az = std::sin(t); }
        a.target.x = a.pos.x + ax * m_cfg.scatterRadius;
        a.target.z = a.pos.z + az * m_cfg.scatterRadius;
        clampToRegion(a.target.x, a.target.z);
        a.state = CrowdState::Scatter;
        a.timer = 2.5f;   // max sprint burst before huddling anyway
    }
    // The ball freezes where it lies (dropped mid-game).
    for (CrowdProp& b : m_balls) {
        b.inFlight = false;
        b.holder = kNoLink;
    }
}

void CrowdSystem::endConverse(CrowdAgent& a) {
    a.partner = kNoLink;
    a.chatCooldown = 8.0f + frand() * 12.0f;
    a.state = CrowdState::Wander;
    const uint32_t npts = (uint32_t)(m_points.size() / 2);
    if (npts > 0) {
        const uint32_t pi = rng() % npts;
        const float ang = (float)(rng() % 6283) * 0.001f;
        a.target.x = m_points[pi * 2 + 0] + std::cos(ang) * 1.2f;
        a.target.z = m_points[pi * 2 + 1] + std::sin(ang) * 1.2f;
        clampToRegion(a.target.x, a.target.z);
    }
}

void CrowdSystem::resumeAfterCalm(CrowdAgent& a) {
    const uint32_t npts = (uint32_t)(m_points.size() / 2);
    switch (a.role) {
    case CrowdRole::Worker:
        // Back to work: walk to the (possibly dropped) crate / console / line.
        a.state = CrowdState::Work;
        a.workPhase = 0;
        a.timer = 0.0f;
        break;
    case CrowdRole::Gamer: {
        // Drift back to the knot; the ball resumes once two players are back.
        a.state = CrowdState::Play;
        a.timer = 0.0f;
        const CrowdPlaySpot& ps = m_cfg.play[a.playIdx];
        const uint32_t want = ps.ball ? std::max(2u, ps.players) : 2u;
        const float ang = 6.2831853f * (float)a.slot / (float)want;
        a.target.x = ps.cx + std::cos(ang) * (ps.ball ? kKnotRadius : 0.55f);
        a.target.z = ps.cz + std::sin(ang) * (ps.ball ? kKnotRadius : 0.55f);
        clampToRegion(a.target.x, a.target.z);
        break; }
    case CrowdRole::Civilian:
    default:
        a.state = CrowdState::Wander;   // return to the hangouts
        if (npts > 0) {
            const uint32_t pi = rng() % npts;
            a.target.x = m_points[pi * 2 + 0];
            a.target.z = m_points[pi * 2 + 1];
            clampToRegion(a.target.x, a.target.z);
        }
        break;
    }
}

// Opportunistic pair-ups: wandering/idling civilians who pass close (and are
// off cooldown) stop for a chat; a third may drift into an ongoing pair.
void CrowdSystem::updateConversePairing() {
    if (!m_cfg.converse) return;
    auto eligible = [&](const CrowdAgent& a) {
        return a.role == CrowdRole::Civilian && a.partner == kNoLink &&
               a.chatCooldown <= 0.0f &&
               (a.state == CrowdState::Idle || a.state == CrowdState::Wander);
    };
    const uint32_t n = (uint32_t)m_agents.size();
    for (uint32_t i = 0; i < n; ++i) {
        CrowdAgent& a = m_agents[i];
        if (!eligible(a)) continue;
        // 1) pair with another eligible civilian passing within ~3 m.
        bool paired = false;
        for (uint32_t j = i + 1; j < n && !paired; ++j) {
            CrowdAgent& b = m_agents[j];
            if (!eligible(b)) continue;
            if (dist2XZ(a.pos, b.pos) > 9.0f) continue;
            // Talk slots: 0.6 m either side of the midpoint => ~1.2 m apart.
            float mx = (a.pos.x + b.pos.x) * 0.5f, mz = (a.pos.z + b.pos.z) * 0.5f;
            float dx = b.pos.x - a.pos.x, dz = b.pos.z - a.pos.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len > 0.05f) { dx /= len; dz /= len; }
            else { dx = 1.0f; dz = 0.0f; }
            a.target.x = mx - dx * 0.6f; a.target.z = mz - dz * 0.6f;
            b.target.x = mx + dx * 0.6f; b.target.z = mz + dz * 0.6f;
            clampToRegion(a.target.x, a.target.z);
            clampToRegion(b.target.x, b.target.z);
            const float talk = 6.0f + frand() * 6.0f;   // same dwell => same-frame part
            a.partner = j; b.partner = i;
            a.state = b.state = CrowdState::Converse;
            a.timer = b.timer = talk;
            paired = true;
        }
        if (paired) continue;
        // 2) sometimes a third joins a pair it wanders past.
        for (uint32_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const CrowdAgent& b = m_agents[j];
            if (b.state != CrowdState::Converse || b.partner == kNoLink) continue;
            if (b.partner >= n || m_agents[b.partner].state != CrowdState::Converse) continue;
            if (dist2XZ(a.pos, b.pos) > 6.25f) continue;   // within 2.5 m
            if ((rng() & 3u) != 0u) { a.chatCooldown = 2.0f; break; }   // usually walks on
            const CrowdAgent& c = m_agents[b.partner];
            const float mx = (b.pos.x + c.pos.x) * 0.5f, mz = (b.pos.z + c.pos.z) * 0.5f;
            float px = -(c.pos.z - b.pos.z), pz = (c.pos.x - b.pos.x);   // perpendicular
            const float pl = std::sqrt(px * px + pz * pz);
            if (pl > 0.05f) { px /= pl; pz /= pl; }
            else { px = 0.0f; pz = 1.0f; }
            // Stand on the side of the pair axis the joiner is already on.
            const float side = ((a.pos.x - mx) * px + (a.pos.z - mz) * pz) >= 0.0f ? 1.0f : -1.0f;
            a.target.x = mx + px * side * 1.0f;
            a.target.z = mz + pz * side * 1.0f;
            clampToRegion(a.target.x, a.target.z);
            a.partner = j;
            a.state = CrowdState::Converse;
            a.timer = 3.0f + frand() * 4.0f;
            break;
        }
    }
}

// Kickabout balls: chip-pass agent-to-agent while the knot is intact + calm.
void CrowdSystem::updateBalls(float dt, Scene& scene) {
    for (uint32_t s = 0; s < (uint32_t)m_balls.size(); ++s) {
        CrowdProp& b = m_balls[s];
        if (b.entity == kNoLink) continue;   // hand-game spot: no ball
        if (m_calmTimer > 0.0f) {
            // Violence is fresh: the ball lies where it stopped.
            writePropTransform(b.entity, scene, b.pos.x, b.pos.y, b.pos.z, 0.0f, 1.0f);
            continue;
        }
        // Who is at the knot and playing?
        uint32_t players[16]; uint32_t np = 0;
        for (uint32_t i = 0; i < (uint32_t)m_agents.size() && np < 16; ++i)
            if (m_agents[i].playIdx == s && m_agents[i].state == CrowdState::Play)
                players[np++] = i;
        if (np < 2) {   // not enough players back yet — ball waits
            writePropTransform(b.entity, scene, b.pos.x, b.pos.y, b.pos.z, 0.0f, 1.0f);
            continue;
        }
        if (b.holder == kNoLink) {
            // (Re)start: pass to whichever player is nearest the ball.
            uint32_t best = players[0]; float bestD = 1e30f;
            for (uint32_t p = 0; p < np; ++p) {
                const float d = dist2XZ(m_agents[players[p]].pos, b.pos);
                if (d < bestD) { bestD = d; best = players[p]; }
            }
            b.holder = best;
            b.from = b.pos;
            b.inFlight = true;
            b.flightT = 0.0f;
            b.flightDur = std::max(0.25f, std::sqrt(bestD) / kPassSpeed);
        }
        // Holder scattered out from under the ball? Freeze until the knot reforms.
        if (b.holder >= (uint32_t)m_agents.size() ||
            m_agents[b.holder].state != CrowdState::Play) {
            b.holder = kNoLink; b.inFlight = false;
            writePropTransform(b.entity, scene, b.pos.x, b.pos.y, b.pos.z, 0.0f, 1.0f);
            continue;
        }
        const CrowdAgent& h = m_agents[b.holder];
        if (b.inFlight) {
            b.flightT += dt;
            const float t = std::min(1.0f, b.flightT / b.flightDur);
            // Home on the receiver's feet (they shuffle while it flies).
            const float fx = h.pos.x - std::sin(h.yaw) * 0.35f;
            const float fz = h.pos.z - std::cos(h.yaw) * 0.35f;
            b.pos.x = b.from.x + (fx - b.from.x) * t;
            b.pos.z = b.from.z + (fz - b.from.z) * t;
            b.pos.y = m_cfg.groundY + std::sin(t * 3.14159265f) * 0.22f;   // chip arc
            if (t >= 1.0f) {
                b.inFlight = false;
                b.pos.y = m_cfg.groundY;
                b.flightT = 0.0f;
                b.flightDur = 1.0f;
                // Dwell at the receiver's feet before the next pass.
                b.from = b.pos;
            }
        } else {
            // Sitting at the holder's feet; kick it on after a beat.
            b.flightT += dt;
            if (b.flightT >= 0.5f + (float)(b.holder % 3) * 0.35f) {
                uint32_t next = b.holder;
                if (np > 1) {
                    for (int tries = 0; tries < 4 && next == b.holder; ++tries)
                        next = players[rng() % np];
                }
                b.holder = next;
                b.from = b.pos;
                b.inFlight = true;
                b.flightT = 0.0f;
                const float d = std::sqrt(dist2XZ(m_agents[next].pos, b.pos));
                b.flightDur = std::max(0.25f, d / kPassSpeed);
            }
        }
        writePropTransform(b.entity, scene, b.pos.x, b.pos.y, b.pos.z, 0.0f, 1.0f);
    }
}

void CrowdSystem::update(float dt, Scene& scene) {
    if (!m_built) return;
    m_time += dt;
    if (m_calmTimer > 0.0f) m_calmTimer -= dt;
    const uint32_t npts = (uint32_t)(m_points.size() / 2);

    updateConversePairing();

    for (uint32_t ai = 0; ai < (uint32_t)m_agents.size(); ++ai) {
        CrowdAgent& a = m_agents[ai];
        if (a.timer > 0.0f) a.timer -= dt;
        if (a.chatCooldown > 0.0f) a.chatCooldown -= dt;
        float bob = 0.0f, crouch = 1.0f, speed = 0.0f, lean = 0.0f;
        bool faceLocked = false;   // a state already steered the yaw

        switch (a.state) {
        case CrowdState::Idle:
            if (m_cfg.dance) {
                // Sway/bob to the beat (per-agent phase so the floor desyncs).
                a.yaw += std::sin(m_time * 2.2f + a.phase) * 0.9f * dt;
                bob = std::fabs(std::sin(m_time * 3.4f + a.phase)) * 0.06f;
            }
            if (a.timer <= 0.0f && npts > 0) {
                // Drift to a fresh knot around another hangout point.
                const uint32_t pi = rng() % npts;
                const float ang = (float)(rng() % 6283) * 0.001f;
                const float rad = 0.6f + (float)(rng() % 1000) * 0.0016f;
                a.target.x = m_points[pi * 2 + 0] + std::cos(ang) * rad;
                a.target.z = m_points[pi * 2 + 1] + std::sin(ang) * rad;
                clampToRegion(a.target.x, a.target.z);
                a.state = CrowdState::Wander;
            }
            break;
        case CrowdState::Wander:
            speed = m_cfg.walkSpeed;
            if (dist2XZ(a.pos, a.target) < 0.25f) {
                a.state = CrowdState::Idle;
                a.timer = m_cfg.dwellMin
                        + (float)(rng() % 1000) * 0.001f * (m_cfg.dwellMax - m_cfg.dwellMin);
            }
            break;
        case CrowdState::Scatter:
            speed = m_cfg.fleeSpeed;
            if (a.timer <= 0.0f || dist2XZ(a.pos, a.target) < 0.25f)
                a.state = CrowdState::Cower;
            break;
        case CrowdState::Cower:
            // Huddled low, trembling-still; stand back up once things are calm.
            crouch = 0.55f + std::sin(m_time * 18.0f + a.phase) * 0.015f;
            if (m_calmTimer <= 0.0f) resumeAfterCalm(a);
            break;

        case CrowdState::Converse: {
            // Partner gone (scattered / parted / never mutual)? Walk on.
            const bool partnerOk = a.partner != kNoLink &&
                                   a.partner < (uint32_t)m_agents.size() &&
                                   m_agents[a.partner].state == CrowdState::Converse;
            if (!partnerOk || a.timer <= 0.0f) { endConverse(a); break; }
            if (dist2XZ(a.pos, a.target) > 0.04f) {
                speed = m_cfg.walkSpeed * 0.85f;   // walk into the talk slot
            } else {
                // Talking: face the partner, nod (phase-offset bob) + a light
                // lean-in gesture. The pair's phases differ so they alternate.
                const CrowdAgent& p = m_agents[a.partner];
                faceToward(a, p.pos.x, p.pos.z, dt, 6.0f);
                faceLocked = true;
                bob  = std::fabs(std::sin(m_time * 2.8f + a.phase)) * 0.03f;
                lean = std::max(0.0f, std::sin(m_time * 1.3f + a.phase)) * 0.07f;
            }
            break; }

        case CrowdState::Work: {
            if (a.workIdx == kNoLink || a.workIdx >= (uint32_t)m_cfg.work.size()) {
                a.state = CrowdState::Idle;
                break;
            }
            const CrowdWorkPoint& wp = m_cfg.work[a.workIdx];
            switch (wp.kind) {
            case CrowdWorkPoint::Kind::Carry: {
                CrowdProp* c = (m_crateForWork[a.workIdx] != kNoLink)
                             ? &m_crates[m_crateForWork[a.workIdx]] : nullptr;
                if (!c) { a.state = CrowdState::Idle; break; }
                switch (a.workPhase) {
                case kCarryToCrate:
                    a.target.x = c->pos.x; a.target.z = c->pos.z;
                    speed = m_cfg.walkSpeed;
                    if (dist2XZ(a.pos, a.target) < 0.20f) {
                        a.workPhase = kCarryHoist; a.timer = 0.7f;
                    }
                    break;
                case kCarryHoist:
                    lean = 0.16f;   // bent over the crate
                    if (a.timer <= 0.0f) { c->carried = true; a.workPhase = kCarryToB; }
                    break;
                case kCarryToB:
                    a.target.x = wp.bx; a.target.z = wp.bz;
                    speed = m_cfg.walkSpeed * 0.9f;
                    lean = 0.13f;   // leaning back under the load
                    if (dist2XZ(a.pos, a.target) < 0.16f) {
                        c->carried = false;
                        c->pos = { wp.bx, m_cfg.groundY, wp.bz };
                        a.workPhase = kCarryDrop; a.timer = 0.8f;
                    }
                    break;
                case kCarryDrop:
                    lean = 0.16f;   // setting it down
                    if (a.timer <= 0.0f) a.workPhase = kCarryReturn;
                    break;
                case kCarryReturn:
                default:
                    a.target.x = wp.ax; a.target.z = wp.az;
                    speed = m_cfg.walkSpeed;
                    if (dist2XZ(a.pos, a.target) < 0.20f) {
                        // Restock: the next crate is waiting back at A.
                        if (!c->carried) c->pos = { wp.ax, m_cfg.groundY, wp.az };
                        a.workPhase = kCarryToCrate;
                    }
                    break;
                }
                // A carried crate rides in the arms (front of the chest).
                if (c->carried) {
                    const float fx = a.pos.x - std::sin(a.yaw) * 0.34f;
                    const float fz = a.pos.z - std::cos(a.yaw) * 0.34f;
                    c->pos = { fx, a.pos.y + 0.72f * m_cfg.scale, fz };
                }
                break; }
            case CrowdWorkPoint::Kind::Console:
                a.target.x = wp.ax; a.target.z = wp.az;
                if (dist2XZ(a.pos, a.target) > 0.09f) {
                    speed = m_cfg.walkSpeed;
                } else {
                    // Tending: face the console, periodic lean-in.
                    faceToward(a, wp.bx, wp.bz, dt, 6.0f);
                    faceLocked = true;
                    lean = std::max(0.0f, std::sin(m_time * 0.9f + a.phase)) * 0.11f;
                    bob  = std::fabs(std::sin(m_time * 1.7f + a.phase)) * 0.012f;
                }
                break;
            case CrowdWorkPoint::Kind::Sweep:
            default:
                a.target.x = (a.workPhase == 0) ? wp.ax : wp.bx;
                a.target.z = (a.workPhase == 0) ? wp.az : wp.bz;
                speed = m_cfg.walkSpeed * 0.55f;   // slow line pacing
                lean = 0.05f;
                if (dist2XZ(a.pos, a.target) < 0.16f) a.workPhase = a.workPhase ? 0 : 1;
                break;
            }
            break; }

        case CrowdState::Play: {
            if (a.playIdx == kNoLink || a.playIdx >= (uint32_t)m_cfg.play.size()) {
                a.state = CrowdState::Idle;
                break;
            }
            const CrowdPlaySpot& ps = m_cfg.play[a.playIdx];
            if (!ps.ball) {
                // Seated hand-game pair: crouched face-to-face, alternating
                // gesture bobs (slot 0/1 are half a cycle apart).
                a.target.x = ps.cx + ((a.slot & 1u) ? 0.55f : -0.55f);
                a.target.z = ps.cz;
                if (dist2XZ(a.pos, a.target) > 0.04f) {
                    speed = m_cfg.walkSpeed * 0.8f;
                } else {
                    crouch = 0.74f;   // seated on the bench
                    faceToward(a, ps.cx + ((a.slot & 1u) ? -0.55f : 0.55f), ps.cz, dt, 6.0f);
                    faceLocked = true;
                    const float ph = (a.slot & 1u) ? 3.14159265f : 0.0f;
                    bob  = std::max(0.0f, std::sin(m_time * 3.0f + ph)) * 0.05f;
                    lean = std::max(0.0f, std::sin(m_time * 3.0f + ph)) * 0.10f;
                }
                break;
            }
            // Kickabout: shuffle/reposition between passes, face the ball.
            if (a.timer <= 0.0f) {
                const uint32_t want = std::max(2u, ps.players);
                const float ang = 6.2831853f * (float)a.slot / (float)want
                                + (frand() - 0.5f) * 0.8f;
                const float rad = kKnotRadius + (frand() - 0.5f) * 1.2f;
                a.target.x = ps.cx + std::cos(ang) * rad;
                a.target.z = ps.cz + std::sin(ang) * rad;
                clampToRegion(a.target.x, a.target.z);
                a.timer = 1.5f + frand() * 2.5f;
            }
            if (dist2XZ(a.pos, a.target) > 0.09f) {
                speed = m_cfg.walkSpeed * 0.9f;
            } else if (m_balls[a.playIdx].entity != kNoLink) {
                const CrowdProp& b = m_balls[a.playIdx];
                faceToward(a, b.pos.x, b.pos.z, dt, 8.0f);
                faceLocked = true;
                bob = std::fabs(std::sin(m_time * 2.2f + a.phase)) * 0.02f;   // on their toes
            }
            break; }
        }

        if (speed > 0.0f) {
            const float dx = a.target.x - a.pos.x, dz = a.target.z - a.pos.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len > 0.05f) {
                const float step = std::min(speed * dt, len);
                a.pos.x += dx / len * step;
                a.pos.z += dz / len * step;
                if (!faceLocked) {
                    // Face the motion (-Z forward convention).
                    float want = std::atan2(-dx, -dz);
                    float d = want - a.yaw;
                    while (d >  3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    const float mx = 10.0f * dt;
                    a.yaw += std::clamp(d, -mx, mx);
                }
            }
        }
        // Mirror the gesture values for the skinned visual layer (crowd_skin.h)
        // before writing the blockout transform — same numbers, one source.
        a.visBob = bob; a.visCrouch = crouch; a.visLean = lean;
        writeTransform(a, scene, bob, crouch, lean);
    }

    // Crate transforms (carried crates were repositioned in the Work case).
    for (CrowdProp& c : m_crates)
        writePropTransform(c.entity, scene, c.pos.x, c.pos.y, c.pos.z, 0.0f, 1.0f);

    updateBalls(dt, scene);
}

// ===========================================================================
// Headless self-test (--test-crowd)
// ===========================================================================

namespace {

int c_pass = 0, c_fail = 0;
void ccheck(bool cond, const char* name) {
    if (cond) { ++c_pass; x3::logInfo(std::string("[crowd-test] PASS ") + name); }
    else      { ++c_fail; x3::logError(std::string("[crowd-test] FAIL ") + name); }
}

class CrowdCountingDevice final : public HeadlessRenderDevice {
public:
    uint32_t meshCreates = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                   const uint32_t* idx, uint32_t ni) override {
        ++meshCreates;
        return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
    }
};

float yawErrorToward(const CrowdAgent& a, float x, float z) {
    const float want = std::atan2(-(x - a.pos.x), -(z - a.pos.z));
    float d = want - a.yaw;
    while (d >  3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return std::fabs(d);
}

} // namespace

bool runCrowdSelfTest() {
    c_pass = c_fail = 0;
    CrowdCountingDevice device;
    Scene scene;
    CrowdSystem crowd;
    CrowdConfig cfg;
    cfg.count = 14;
    cfg.centerX = 0.0f; cfg.centerZ = 0.0f; cfg.groundY = 0.0f;
    cfg.radius = 12.0f;
    cfg.dance = true;
    crowd.build(cfg, scene, device);

    const float dt = 1.0f / 60.0f;
    auto tick = [&](int frames) { for (int f = 0; f < frames; ++f) crowd.update(dt, scene); };

    // ---- C1: build counts + one shared mesh ----
    ccheck(crowd.agentCount() == 14 && scene.size() == 14 && device.meshCreates == 1,
           "C1 build counts (14 NPCs, one shared mesh)");

    // ---- C2: the idling/wandering crowd stays inside its region ----
    tick(60 * 30);
    {
        bool inside = true;
        for (uint32_t i = 0; i < crowd.agentCount(); ++i) {
            const CrowdAgent& a = crowd.agent(i);
            const float d2 = a.pos.x * a.pos.x + a.pos.z * a.pos.z;
            if (d2 > (cfg.radius + 0.5f) * (cfg.radius + 0.5f)) inside = false;
        }
        ccheck(inside && crowd.calm(), "C2 crowd stays inside its region (30 s)");
    }

    // ---- C3: violence scatters nearby agents AWAY from the point ----
    {
        const x3::phys::Vec3 bang{0.0f, 0.0f, 0.0f};   // center of the floor
        // Record pre-violence distances to the bang.
        std::vector<float> d0(crowd.agentCount());
        for (uint32_t i = 0; i < crowd.agentCount(); ++i)
            d0[i] = dist2XZ(crowd.agent(i).pos, bang);
        crowd.onViolence(bang);
        const uint32_t scattered = crowd.countInState(CrowdState::Scatter);
        tick(45);   // 0.75 s of sprinting
        bool away = true;
        for (uint32_t i = 0; i < crowd.agentCount(); ++i) {
            const CrowdAgent& a = crowd.agent(i);
            if (a.state != CrowdState::Scatter && a.state != CrowdState::Cower) continue;
            if (dist2XZ(a.pos, bang) + 0.01f < d0[i]) away = false;   // moved closer?!
        }
        ccheck(scattered == crowd.agentCount() && away,
               "C3 violence scatters the crowd away from the point");
    }

    // ---- C4: scattered agents settle into Cower while violence is fresh ----
    {
        crowd.onViolence(x3::phys::Vec3{0, 0, 0});   // keep it fresh
        tick(60 * 4);                                 // bursts end (<= 2.5 s)
        ccheck(!crowd.calm() || crowd.countInState(CrowdState::Cower) > 0,
               "C4 scattered crowd cowers while violence is fresh");
        ccheck(crowd.countInState(CrowdState::Cower) == crowd.agentCount(),
               "C4b every scattered agent reached Cower");
    }

    // ---- C5: after calmTime of quiet the crowd returns ----
    {
        tick((int)((cfg.calmTime + 4.0f) * 60.0f));
        const uint32_t back = crowd.countInState(CrowdState::Idle)
                            + crowd.countInState(CrowdState::Wander);
        bool inside = true;
        for (uint32_t i = 0; i < crowd.agentCount(); ++i) {
            const CrowdAgent& a = crowd.agent(i);
            const float d2 = a.pos.x * a.pos.x + a.pos.z * a.pos.z;
            if (d2 > (cfg.radius + 0.5f) * (cfg.radius + 0.5f)) inside = false;
        }
        ccheck(crowd.calm() && back == crowd.agentCount() && inside,
               "C5 crowd returns to idle/wander after calm");
    }

    // ---- C6: leak canary ----
    {
        const uint32_t meshesBefore = device.meshCreates;
        const uint32_t entsBefore = scene.size();
        for (int f = 0; f < 60 * 60; ++f) {
            crowd.update(dt, scene);
            if (f % 600 == 0) crowd.onViolence(x3::phys::Vec3{2, 0, 2});
        }
        ccheck(device.meshCreates == meshesBefore && scene.size() == entsBefore
                   && crowd.agentCount() == 14,
               "C6 leak/budget (no new meshes/entities across long ticking)");
    }

    // =======================================================================
    // LIVING NPCs — converse / work / play (a fresh deployment: 3 workers on
    // carry/console/sweep points, a 3-player kickabout knot, 6 civilians with
    // conversations on).
    // =======================================================================
    CrowdCountingDevice dev2;
    Scene s2;
    CrowdSystem c2;
    CrowdConfig cfg2;
    cfg2.count = 12;
    cfg2.centerX = 0.0f; cfg2.centerZ = 0.0f; cfg2.groundY = 0.0f;
    cfg2.radius = 22.0f;
    cfg2.converse = true;
    cfg2.scatterRadius = 60.0f;   // one bang reaches the whole deployment
    cfg2.points = { -4.0f, 0.0f,  4.0f, 0.0f,  0.0f, 4.0f,  0.0f, -4.0f };
    {
        CrowdWorkPoint carry;  carry.kind = CrowdWorkPoint::Kind::Carry;
        carry.ax = -9.0f; carry.az = -9.0f; carry.bx = -3.0f; carry.bz = -9.0f;
        CrowdWorkPoint cons;   cons.kind = CrowdWorkPoint::Kind::Console;
        cons.ax = 9.0f; cons.az = -9.0f; cons.bx = 9.0f; cons.bz = -11.0f;
        CrowdWorkPoint sweep;  sweep.kind = CrowdWorkPoint::Kind::Sweep;
        sweep.ax = -9.0f; sweep.az = 9.0f; sweep.bx = -3.0f; sweep.bz = 9.0f;
        cfg2.work = { carry, cons, sweep };
        CrowdPlaySpot knot; knot.cx = 8.0f; knot.cz = 8.0f; knot.players = 3; knot.ball = true;
        cfg2.play = { knot };
    }
    c2.build(cfg2, s2, dev2);
    auto tick2 = [&](int frames) { for (int f = 0; f < frames; ++f) c2.update(dt, s2); };

    ccheck(c2.agentCount() == 12 && c2.crateCount() == 1 && c2.ballCount() == 1 &&
           c2.ball(0).entity != kNoLink && s2.size() == 14,
           "C0b living deployment builds (12 NPCs + 1 crate + 1 ball)");

    // ---- C7: civilians pair into conversations, ~1.2 m apart, FACING each other ----
    {
        bool paired = false;
        int  frames = 0;
        for (; frames < 60 * 90 && !paired; ++frames) {
            c2.update(dt, s2);
            paired = c2.countInState(CrowdState::Converse) >= 2;
        }
        // Let the pair walk into its talk slots and settle into the chat.
        tick2(60 * 3);
        bool facing = false, spaced = false, mutualFound = false;
        for (uint32_t i = 0; i < c2.agentCount() && !mutualFound; ++i) {
            const CrowdAgent& a = c2.agent(i);
            if (a.state != CrowdState::Converse || a.partner == kNoLink) continue;
            const CrowdAgent& b = c2.agent(a.partner);
            if (b.state != CrowdState::Converse || b.partner != i) continue;
            mutualFound = true;
            const float d = std::sqrt(dist2XZ(a.pos, b.pos));
            spaced = d > 0.6f && d < 2.4f;
            facing = yawErrorToward(a, b.pos.x, b.pos.z) < 0.4f &&
                     yawErrorToward(b, a.pos.x, a.pos.z) < 0.4f;
        }
        // Pairs part after their dwell, so re-check membership loosely: pairing
        // HAPPENED and the sampled pair (if still talking) stood right + faced.
        ccheck(paired && (!mutualFound || (spaced && facing)),
               "C7 conversations form (pair ~1.2 m apart, facing each other)");
        ccheck(mutualFound && spaced && facing,
               "C7b a live mutual pair sampled mid-chat (spacing + facing verified)");
    }

    // ---- C8: the work loops reach their points ----
    {
        // Identify the workers by their work kinds.
        uint32_t carrier = kNoLink, tender = kNoLink, sweeper = kNoLink;
        for (uint32_t i = 0; i < c2.agentCount(); ++i) {
            const CrowdAgent& a = c2.agent(i);
            if (a.role != CrowdRole::Worker) continue;
            switch (cfg2.work[a.workIdx].kind) {
            case CrowdWorkPoint::Kind::Carry:   carrier = i; break;
            case CrowdWorkPoint::Kind::Console: tender  = i; break;
            case CrowdWorkPoint::Kind::Sweep:   sweeper = i; break;
            }
        }
        bool carrierAtA = false, carrierAtB = false, crateAtB = false, crateRode = false;
        bool tenderAt = false, sweepA = false, sweepB = false;
        for (int f = 0; f < 60 * 60; ++f) {
            c2.update(dt, s2);
            const CrowdAgent& ca = c2.agent(carrier);
            const x3::phys::Vec3 A{cfg2.work[0].ax, 0, cfg2.work[0].az};
            const x3::phys::Vec3 B{cfg2.work[0].bx, 0, cfg2.work[0].bz};
            if (dist2XZ(ca.pos, A) < 0.5f) carrierAtA = true;
            if (dist2XZ(ca.pos, B) < 0.5f) carrierAtB = true;
            const CrowdProp& crate = c2.crate(0);
            if (crate.carried) crateRode = true;
            if (!crate.carried && dist2XZ(crate.pos, B) < 0.4f) crateAtB = true;
            const CrowdAgent& tn = c2.agent(tender);
            if (dist2XZ(tn.pos, x3::phys::Vec3{cfg2.work[1].ax, 0, cfg2.work[1].az}) < 0.3f)
                tenderAt = true;
            const CrowdAgent& sw = c2.agent(sweeper);
            if (dist2XZ(sw.pos, x3::phys::Vec3{cfg2.work[2].ax, 0, cfg2.work[2].az}) < 0.4f) sweepA = true;
            if (dist2XZ(sw.pos, x3::phys::Vec3{cfg2.work[2].bx, 0, cfg2.work[2].bz}) < 0.4f) sweepB = true;
        }
        ccheck(carrier != kNoLink && carrierAtA && carrierAtB && crateRode && crateAtB,
               "C8 crate-carry loop: worker walks A->B, the crate rides and lands at B");
        ccheck(tender != kNoLink && tenderAt, "C8b console-tend holds its point");
        ccheck(sweeper != kNoLink && sweepA && sweepB, "C8c sweep paces both line ends");
    }

    // ---- C9: the kickabout ball passes between distinct players ----
    {
        uint32_t holders[3] = { kNoLink, kNoLink, kNoLink };
        uint32_t nHolders = 0;
        bool inKnot = true;
        for (int f = 0; f < 60 * 90; ++f) {
            c2.update(dt, s2);
            const CrowdProp& b = c2.ball(0);
            if (b.holder != kNoLink) {
                bool known = false;
                for (uint32_t h = 0; h < nHolders; ++h) if (holders[h] == b.holder) known = true;
                if (!known && nHolders < 3) holders[nHolders++] = b.holder;
            }
            const float dx = b.pos.x - 8.0f, dz = b.pos.z - 8.0f;
            if (dx * dx + dz * dz > 8.0f * 8.0f) inKnot = false;
        }
        ccheck(nHolders >= 2 && inKnot,
               "C9 kickabout: the ball passes between distinct players and stays in the knot");
    }

    // ---- C10: violence scatters EVERY state (chat/work/play all drop) ----
    {
        c2.onViolence(x3::phys::Vec3{0.0f, 0.0f, 0.0f});
        const uint32_t hit = c2.countInState(CrowdState::Scatter)
                           + c2.countInState(CrowdState::Cower);
        tick2(60 * 4);
        const uint32_t settled = c2.countInState(CrowdState::Scatter)
                               + c2.countInState(CrowdState::Cower);
        const bool crateDropped = !c2.crate(0).carried;
        const bool ballFrozen = !c2.ball(0).inFlight;
        ccheck(hit == c2.agentCount() && settled == c2.agentCount(),
               "C10 violence scatters EVERY state (converse/work/play included)");
        ccheck(crateDropped && ballFrozen, "C10b the crate is dropped, the ball freezes");
    }

    // ---- C11: after calm, life resumes by role ----
    {
        tick2((int)((cfg2.calmTime + 20.0f) * 60.0f));
        bool rolesResumed = true, inside = true;
        for (uint32_t i = 0; i < c2.agentCount(); ++i) {
            const CrowdAgent& a = c2.agent(i);
            if (a.role == CrowdRole::Worker && a.state != CrowdState::Work) rolesResumed = false;
            if (a.role == CrowdRole::Gamer  && a.state != CrowdState::Play) rolesResumed = false;
            if (a.role == CrowdRole::Civilian &&
                a.state != CrowdState::Idle && a.state != CrowdState::Wander &&
                a.state != CrowdState::Converse) rolesResumed = false;
            const float d2 = a.pos.x * a.pos.x + a.pos.z * a.pos.z;
            if (d2 > (cfg2.radius + 0.5f) * (cfg2.radius + 0.5f)) inside = false;
        }
        // The ball starts moving again (a holder is re-established).
        bool ballBack = false;
        for (int f = 0; f < 60 * 10 && !ballBack; ++f) {
            c2.update(dt, s2);
            ballBack = c2.ball(0).holder != kNoLink;
        }
        ccheck(c2.calm() && rolesResumed && inside,
               "C11 after calm: workers work, players play, civilians wander (in-region)");
        ccheck(ballBack, "C11b the kickabout resumes after calm");
    }

    // ---- C12: extended-system leak canary ----
    {
        const uint32_t meshesBefore = dev2.meshCreates;
        const uint32_t entsBefore = s2.size();
        for (int f = 0; f < 60 * 60; ++f) {
            c2.update(dt, s2);
            if (f % 700 == 0) c2.onViolence(x3::phys::Vec3{2, 0, 2});
        }
        ccheck(dev2.meshCreates == meshesBefore && s2.size() == entsBefore &&
               c2.agentCount() == 12,
               "C12 living deployment leaks nothing across long ticking");
    }

    // ---- C13: abandon() forgets without touching the scene; rebuild works ----
    {
        const uint32_t entsBefore = s2.size();
        const uint32_t meshesBefore = dev2.meshCreates;
        c2.abandon();
        c2.update(dt, s2);                       // must be a no-op
        c2.onViolence(x3::phys::Vec3{0, 0, 0});  // must be a no-op
        const bool forgot = !c2.built() && c2.agentCount() == 0 &&
                            c2.crateCount() == 0 && c2.ballCount() == 0 &&
                            s2.size() == entsBefore && dev2.meshCreates == meshesBefore;
        // A rebuild after abandon works (the region re-realize path).
        c2.build(cfg2, s2, dev2);
        tick2(60);
        ccheck(forgot && c2.built() && c2.agentCount() == 12,
               "C13 abandon() is scene-safe (ledger owns teardown) + rebuild works");
    }

    x3::logInfo("crowd: " + std::to_string(c_pass) + "/"
                + std::to_string(c_pass + c_fail) + " passed");

    // ---- SKINNED CITIZENS layer (S1..S5, app/crowd_skin.cpp): the skinned
    // visual layer binds/falls back/streams over the same brains. ----
    const bool skinOk = runCrowdSkinSelfTest();

    return c_fail == 0 && skinOk;
}

} // namespace x3::game
