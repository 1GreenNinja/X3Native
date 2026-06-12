// CROWDS implementation — see app/crowd.h for the design stance.
// Game/slice code only — engine/ stays pure.

#include "crowd.h"
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

const char* crowdStateName(CrowdState s) {
    switch (s) {
        case CrowdState::Idle:    return "Idle";
        case CrowdState::Wander:  return "Wander";
        case CrowdState::Scatter: return "Scatter";
        case CrowdState::Cower:   return "Cower";
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

} // namespace

uint32_t CrowdSystem::rng() {
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return m_rngState;
}

void CrowdSystem::clampToRegion(float& x, float& z) const {
    const float dx = x - m_cfg.centerX, dz = z - m_cfg.centerZ;
    const float d2 = dx * dx + dz * dz;
    const float r  = m_cfg.radius;
    if (d2 <= r * r || d2 < 1e-6f) return;
    const float d = std::sqrt(d2);
    x = m_cfg.centerX + dx / d * r;
    z = m_cfg.centerZ + dz / d * r;
}

void CrowdSystem::writeTransform(CrowdAgent& a, Scene& scene, float bob, float crouch) {
    if (a.entity == kNoLink) return;
    Entity& e = scene.get(a.entity);
    const float s  = m_cfg.scale;
    const float sy = s * crouch;
    const float cy = std::cos(a.yaw), sn = std::sin(a.yaw);
    float* t = e.transform;
    t[0] = cy * s;  t[1] = 0;   t[2]  = -sn * s; t[3]  = 0;
    t[4] = 0;       t[5] = sy;  t[6]  = 0;       t[7]  = 0;
    t[8] = sn * s;  t[9] = 0;   t[10] = cy * s;  t[11] = 0;
    t[12] = a.pos.x; t[13] = a.pos.y + bob; t[14] = a.pos.z; t[15] = 1;
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

    const uint32_t npts = (uint32_t)(m_points.size() / 2);
    for (uint32_t k = 0; k < m_cfg.count; ++k) {
        CrowdAgent a;
        // Seed in a loose knot around one of the points.
        const uint32_t pi = (npts > 0) ? (k % npts) : 0;
        const float px = (npts > 0) ? m_points[pi * 2 + 0] : m_cfg.centerX;
        const float pz = (npts > 0) ? m_points[pi * 2 + 1] : m_cfg.centerZ;
        const float ang = (float)(rng() % 6283) * 0.001f;
        const float rad = 0.6f + (float)(rng() % 1000) * 0.0016f;   // 0.6..2.2 m
        a.pos.x = px + std::cos(ang) * rad;
        a.pos.z = pz + std::sin(ang) * rad;
        clampToRegion(a.pos.x, a.pos.z);
        a.pos.y = m_cfg.groundY;
        a.target = a.pos;
        a.yaw   = (float)(rng() % 6283) * 0.001f;
        a.phase = (float)(rng() % 6283) * 0.001f;
        a.timer = m_cfg.dwellMin
                + (float)(rng() % 1000) * 0.001f * (m_cfg.dwellMax - m_cfg.dwellMin);

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
        a.entity = scene.add(e);
        writeTransform(a, scene, 0.0f, 1.0f);
        m_agents.push_back(a);
    }
    m_built = true;
    x3::logInfo("crowd: built " + std::to_string(m_agents.size()) + " NPCs");
}

uint32_t CrowdSystem::countInState(CrowdState s) const {
    uint32_t n = 0;
    for (const CrowdAgent& a : m_agents) if (a.state == s) ++n;
    return n;
}

void CrowdSystem::onViolence(const x3::phys::Vec3& pos) {
    if (!m_built) return;
    m_calmTimer = m_cfg.calmTime;
    const float r2 = m_cfg.scatterRadius * m_cfg.scatterRadius;
    for (CrowdAgent& a : m_agents) {
        if (dist2XZ(a.pos, pos) > r2) continue;
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
}

void CrowdSystem::update(float dt, Scene& scene) {
    if (!m_built) return;
    m_time += dt;
    if (m_calmTimer > 0.0f) m_calmTimer -= dt;
    const uint32_t npts = (uint32_t)(m_points.size() / 2);

    for (CrowdAgent& a : m_agents) {
        if (a.timer > 0.0f) a.timer -= dt;
        float bob = 0.0f, crouch = 1.0f, speed = 0.0f;

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
            if (m_calmTimer <= 0.0f) {
                a.state = CrowdState::Wander;   // return to the hangouts
                if (npts > 0) {
                    const uint32_t pi = rng() % npts;
                    a.target.x = m_points[pi * 2 + 0];
                    a.target.z = m_points[pi * 2 + 1];
                    clampToRegion(a.target.x, a.target.z);
                }
            }
            break;
        }

        if (speed > 0.0f) {
            const float dx = a.target.x - a.pos.x, dz = a.target.z - a.pos.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len > 0.05f) {
                const float step = std::min(speed * dt, len);
                a.pos.x += dx / len * step;
                a.pos.z += dz / len * step;
                // Face the motion (-Z forward convention).
                float want = std::atan2(-dx, -dz);
                float d = want - a.yaw;
                while (d >  3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                const float mx = 10.0f * dt;
                a.yaw += std::clamp(d, -mx, mx);
            }
        }
        writeTransform(a, scene, bob, crouch);
    }
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

    x3::logInfo("crowd: " + std::to_string(c_pass) + "/"
                + std::to_string(c_pass + c_fail) + " passed");
    return c_fail == 0;
}

} // namespace x3::game
