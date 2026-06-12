// AMBIENT ECOLOGY implementation — see app/ecology.h for the design stance.
// Game/slice code only — engine/ stays pure.

#include "ecology.h"
#include "asset_root.h"     // assetRoot() -> assets/world/ecology.json
#include "json_mini.h"      // tiny shared JSON DOM (ecology.json / alert.json)
#include "mesh_prims.h"     // compound box species meshes
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace x3::game {

// ===========================================================================
// Names
// ===========================================================================

const char* ecoArchetypeName(EcoArchetype a) {
    switch (a) {
        case EcoArchetype::Grazer:   return "grazer";
        case EcoArchetype::Predator: return "predator";
        case EcoArchetype::Patrol:   return "patrol";
    }
    return "?";
}

const char* ecoScheduleName(EcoSchedule s) {
    switch (s) {
        case EcoSchedule::Always: return "always";
        case EcoSchedule::Day:    return "day";
        case EcoSchedule::Night:  return "night";
    }
    return "?";
}

const char* ecoStateName(EcoState s) {
    switch (s) {
        case EcoState::Graze:       return "Graze";
        case EcoState::Wander:      return "Wander";
        case EcoState::Flee:        return "Flee";
        case EcoState::Prowl:       return "Prowl";
        case EcoState::Stalk:       return "Stalk";
        case EcoState::Strike:      return "Strike";
        case EcoState::Feed:        return "Feed";
        case EcoState::Down:        return "Down";
        case EcoState::PatrolMove:  return "PatrolMove";
        case EcoState::PatrolHold:  return "PatrolHold";
        case EcoState::Investigate: return "Investigate";
        case EcoState::OffDuty:     return "OffDuty";
    }
    return "?";
}

// ===========================================================================
// Default cast + JSON loader
// ===========================================================================

EcoConfig defaultEcoConfig() {
    EcoConfig cfg;   // global tunables: the header defaults ARE the tuning

    {   // Shardhorn grazer herd (Keth'zar surface fauna, the bestiary's herd prey)
        EcoSpecies s;
        s.name = "Shardhorn Grazer";
        s.archetype = EcoArchetype::Grazer;
        s.count = 10;
        s.regionX = 30.0f; s.regionZ = 0.0f; s.radius = 45.0f;
        s.speed = 1.4f; s.fastSpeed = 6.0f; s.scale = 1.0f;
        s.color[0] = 0.62f; s.color[1] = 0.52f; s.color[2] = 0.38f;
        cfg.species.push_back(s);
    }
    {   // Crystal Stalker — bestiary A2/L8 ambush predator, prowling the herd edge
        EcoSpecies s;
        s.name = "Crystal Stalker";
        s.archetype = EcoArchetype::Predator;
        s.count = 2;
        s.regionX = 55.0f; s.regionZ = 12.0f; s.radius = 60.0f;
        s.speed = 1.8f; s.fastSpeed = 6.5f; s.scale = 1.0f;
        s.color[0] = 0.30f; s.color[1] = 0.22f; s.color[2] = 0.40f;
        cfg.species.push_back(s);
    }
    {   // Dominion day-shift patrol (facility perimeter route)
        EcoSpecies s;
        s.name = "Dominion Patrol (Day)";
        s.archetype = EcoArchetype::Patrol;
        s.schedule = EcoSchedule::Day;
        s.count = 3;
        s.regionX = -35.0f; s.regionZ = -20.0f; s.radius = 22.0f;
        s.speed = 1.7f; s.fastSpeed = 3.6f; s.scale = 1.0f;
        s.color[0] = 0.30f; s.color[1] = 0.34f; s.color[2] = 0.40f;
        cfg.species.push_back(s);
    }
    {   // Dominion night-shift patrol (same route, darker kit)
        EcoSpecies s;
        s.name = "Dominion Patrol (Night)";
        s.archetype = EcoArchetype::Patrol;
        s.schedule = EcoSchedule::Night;
        s.count = 3;
        s.regionX = -35.0f; s.regionZ = -20.0f; s.radius = 22.0f;
        s.speed = 1.7f; s.fastSpeed = 3.6f; s.scale = 1.0f;
        s.color[0] = 0.34f; s.color[1] = 0.26f; s.color[2] = 0.28f;
        cfg.species.push_back(s);
    }
    return cfg;
}

std::string ecologyJsonPath() { return assetRoot() + "/world/ecology.json"; }

EcoConfig loadEcologyConfig(std::string_view jsonPath) {
    using namespace jmini;
    const std::string path(jsonPath);
    const std::string text = readFile(path);
    if (text.empty()) {
        x3::logInfo("ecology: no config at " + path + " — using the built-in default cast");
        return defaultEcoConfig();
    }
    JReader r(text);
    JVal root = r.parse();
    if (!r.ok || root.t != JVal::Obj) {
        x3::logError("ecology: unparseable " + path + " — using the built-in default cast");
        return defaultEcoConfig();
    }

    EcoConfig cfg;   // header defaults, overridden per present key
    cfg.activeRadius      = root.fnum("activeRadius",      cfg.activeRadius);
    cfg.despawnRadius     = root.fnum("despawnRadius",     cfg.despawnRadius);
    cfg.decisionsPerFrame = (uint32_t)root.inum("decisionsPerFrame", (int)cfg.decisionsPerFrame);
    cfg.cohesionRadius    = root.fnum("cohesionRadius",    cfg.cohesionRadius);
    cfg.separation        = root.fnum("separation",        cfg.separation);
    cfg.fleeRadius        = root.fnum("fleeRadius",        cfg.fleeRadius);
    cfg.playerFleeRadius  = root.fnum("playerFleeRadius",  cfg.playerFleeRadius);
    cfg.fleeTime          = root.fnum("fleeTime",          cfg.fleeTime);
    cfg.panicRadius       = root.fnum("panicRadius",       cfg.panicRadius);
    cfg.grazeTimeMin      = root.fnum("grazeTimeMin",      cfg.grazeTimeMin);
    cfg.grazeTimeMax      = root.fnum("grazeTimeMax",      cfg.grazeTimeMax);
    cfg.stalkRadius       = root.fnum("stalkRadius",       cfg.stalkRadius);
    cfg.strikeRadius      = root.fnum("strikeRadius",      cfg.strikeRadius);
    cfg.killRadius        = root.fnum("killRadius",        cfg.killRadius);
    cfg.strikeSpeedMul    = root.fnum("strikeSpeedMul",    cfg.strikeSpeedMul);
    cfg.feedTime          = root.fnum("feedTime",          cfg.feedTime);
    cfg.downRespawnTime   = root.fnum("downRespawnTime",   cfg.downRespawnTime);
    cfg.playerAggroRadius = root.fnum("playerAggroRadius", cfg.playerAggroRadius);
    cfg.territoryRadius   = root.fnum("territoryRadius",   cfg.territoryRadius);
    cfg.waypointArrive    = root.fnum("waypointArrive",    cfg.waypointArrive);
    cfg.holdTime          = root.fnum("holdTime",          cfg.holdTime);
    cfg.investigateTime   = root.fnum("investigateTime",   cfg.investigateTime);
    cfg.alertSpeedMul     = root.fnum("alertSpeedMul",     cfg.alertSpeedMul);
    cfg.groundY           = root.fnum("groundY",           cfg.groundY);
    if (cfg.despawnRadius <= cfg.activeRadius) cfg.despawnRadius = cfg.activeRadius + 10.0f;
    if (cfg.decisionsPerFrame == 0) cfg.decisionsPerFrame = 1;

    const JVal* species = root.get("species");
    if (species && species->t == JVal::Arr) {
        for (const JVal& js : species->arr) {
            if (js.t != JVal::Obj) continue;
            EcoSpecies s;
            s.name = js.sval("name", s.name);
            const std::string arch = js.sval("archetype", "grazer");
            s.archetype = (arch == "predator") ? EcoArchetype::Predator
                        : (arch == "patrol")   ? EcoArchetype::Patrol
                                               : EcoArchetype::Grazer;
            const std::string sch = js.sval("schedule", "always");
            s.schedule = (sch == "day")   ? EcoSchedule::Day
                       : (sch == "night") ? EcoSchedule::Night
                                          : EcoSchedule::Always;
            s.count     = (uint32_t)std::max(0, js.inum("count", (int)s.count));
            if (const JVal* reg = js.get("region"); reg && reg->t == JVal::Arr && reg->arr.size() >= 2) {
                s.regionX = (float)reg->arr[0].num;
                s.regionZ = (float)reg->arr[1].num;
            }
            s.radius    = js.fnum("radius",    s.radius);
            s.speed     = js.fnum("speed",     s.speed);
            s.fastSpeed = js.fnum("fastSpeed", s.fastSpeed);
            s.scale     = js.fnum("scale",     s.scale);
            if (const JVal* col = js.get("color"); col && col->t == JVal::Arr && col->arr.size() >= 3)
                for (int k = 0; k < 3; ++k) s.color[k] = (float)col->arr[k].num;
            if (const JVal* wps = js.get("waypoints"); wps && wps->t == JVal::Arr) {
                for (const JVal& wp : wps->arr)
                    if (wp.t == JVal::Arr && wp.arr.size() >= 2) {
                        s.waypoints.push_back((float)wp.arr[0].num);
                        s.waypoints.push_back((float)wp.arr[1].num);
                    }
            }
            cfg.species.push_back(s);
        }
    }
    if (cfg.species.empty()) {
        x3::logError("ecology: " + path + " has no species — using the built-in default cast");
        return defaultEcoConfig();
    }
    x3::logInfo("ecology: loaded " + std::to_string(cfg.species.size()) + " species from " + path);
    return cfg;
}

// ===========================================================================
// Species meshes — one shared procedural compound mesh per species (readable
// boxy critters; the SIMULACRA stance from the header).
// ===========================================================================

namespace {

// Append a local-space box to a compound mesh (render-only — agents carry no
// collision; index-offset merge of mesh_prims::makeBox output).
void appendBox(x3::prims::PrimMesh& dst, float hx, float hy, float hz,
               float cx, float cy, float cz) {
    x3::prims::PrimMesh b = x3::prims::makeBox(hx, hy, hz, cx, cy, cz);
    const uint32_t base = (uint32_t)dst.verts.size();
    dst.verts.insert(dst.verts.end(), b.verts.begin(), b.verts.end());
    for (uint32_t i : b.index) dst.index.push_back(base + i);
}

// Quadruped grazer: barrel body, head low at the front (-Z forward), 4 legs and
// the namesake shard-horn ridge.
x3::prims::PrimMesh makeGrazerMesh() {
    x3::prims::PrimMesh m;
    appendBox(m, 0.45f, 0.30f, 0.60f,  0.00f, 0.65f,  0.05f);   // body barrel
    appendBox(m, 0.18f, 0.17f, 0.24f,  0.00f, 0.80f, -0.78f);   // head
    appendBox(m, 0.05f, 0.22f, 0.05f,  0.00f, 1.08f, -0.80f);   // shard horn
    appendBox(m, 0.08f, 0.33f, 0.08f, -0.30f, 0.33f, -0.42f);   // legs
    appendBox(m, 0.08f, 0.33f, 0.08f,  0.30f, 0.33f, -0.42f);
    appendBox(m, 0.08f, 0.33f, 0.08f, -0.30f, 0.33f,  0.48f);
    appendBox(m, 0.08f, 0.33f, 0.08f,  0.30f, 0.33f,  0.48f);
    return m;
}

// Low-slung predator: long body, jaw block forward, tail, crystal back spikes.
x3::prims::PrimMesh makePredatorMesh() {
    x3::prims::PrimMesh m;
    appendBox(m, 0.34f, 0.22f, 0.70f,  0.00f, 0.45f,  0.05f);   // body
    appendBox(m, 0.22f, 0.16f, 0.30f,  0.00f, 0.42f, -0.95f);   // jaws/head
    appendBox(m, 0.10f, 0.08f, 0.35f,  0.00f, 0.45f,  0.95f);   // tail
    appendBox(m, 0.06f, 0.18f, 0.06f,  0.00f, 0.78f, -0.20f);   // crystal spikes
    appendBox(m, 0.06f, 0.14f, 0.06f,  0.00f, 0.74f,  0.20f);
    appendBox(m, 0.07f, 0.24f, 0.07f, -0.26f, 0.24f, -0.55f);   // legs
    appendBox(m, 0.07f, 0.24f, 0.07f,  0.26f, 0.24f, -0.55f);
    appendBox(m, 0.07f, 0.24f, 0.07f, -0.26f, 0.24f,  0.55f);
    appendBox(m, 0.07f, 0.24f, 0.07f,  0.26f, 0.24f,  0.55f);
    return m;
}

// Humanoid patrol trooper: torso/head/legs/arms blocks (graybox-faithful).
x3::prims::PrimMesh makePatrolMesh() {
    x3::prims::PrimMesh m;
    appendBox(m, 0.22f, 0.34f, 0.13f,  0.00f, 1.06f, 0.00f);    // torso
    appendBox(m, 0.11f, 0.12f, 0.11f,  0.00f, 1.58f, 0.00f);    // head
    appendBox(m, 0.09f, 0.40f, 0.09f, -0.12f, 0.40f, 0.00f);    // legs
    appendBox(m, 0.09f, 0.40f, 0.09f,  0.12f, 0.40f, 0.00f);
    appendBox(m, 0.07f, 0.31f, 0.07f, -0.30f, 1.05f, 0.00f);    // arms
    appendBox(m, 0.07f, 0.31f, 0.07f,  0.30f, 1.05f, 0.00f);
    appendBox(m, 0.05f, 0.05f, 0.22f,  0.20f, 1.18f, -0.22f);   // slung rifle
    return m;
}

inline float dist2XZ(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

// Slew an angle toward a target (shortest arc), rate-limited.
inline float slewYaw(float yaw, float target, float maxStep) {
    float d = target - yaw;
    while (d >  3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    if (d >  maxStep) d =  maxStep;
    if (d < -maxStep) d = -maxStep;
    return yaw + d;
}

} // namespace

// ===========================================================================
// AmbientEcology
// ===========================================================================

uint32_t AmbientEcology::rng() {
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return m_rngState;
}

float AmbientEcology::groundAt(float x, float z) const {
    return m_groundFn ? m_groundFn(x, z) : m_cfg.groundY;
}

void AmbientEcology::writeTransform(EcoAgent& a, Scene& scene) {
    if (a.entity == kNoLink) return;
    Entity& e = scene.get(a.entity);
    const float s  = m_cfg.species[a.species].scale;
    const float cy = std::cos(a.yaw) * s, sy = std::sin(a.yaw) * s;
    float* t = e.transform;
    // Column-major yaw-about-Y * uniform scale, translation in column 3.
    t[0] = cy;  t[1] = 0;  t[2]  = -sy; t[3]  = 0;
    t[4] = 0;   t[5] = s;  t[6]  = 0;   t[7]  = 0;
    t[8] = sy;  t[9] = 0;  t[10] = cy;  t[11] = 0;
    t[12] = a.pos.x; t[13] = a.pos.y; t[14] = a.pos.z; t[15] = 1;
}

void AmbientEcology::build(const EcoConfig& cfg, Scene& scene,
                           x3::rhi::IRenderDevice& device) {
    if (m_built) return;
    m_cfg = cfg;
    if (m_cfg.despawnRadius <= m_cfg.activeRadius)
        m_cfg.despawnRadius = m_cfg.activeRadius + 10.0f;
    if (m_cfg.decisionsPerFrame == 0) m_cfg.decisionsPerFrame = 1;

    const uint32_t ns = (uint32_t)m_cfg.species.size();
    m_speciesMesh.resize(ns);
    m_routes.resize(ns);
    m_centroidX.assign(ns, 0.0f);
    m_centroidZ.assign(ns, 0.0f);
    m_centroidN.assign(ns, 0u);

    for (uint32_t si = 0; si < ns; ++si) {
        const EcoSpecies& sp = m_cfg.species[si];

        // One shared procedural mesh per species.
        x3::prims::PrimMesh pm =
            sp.archetype == EcoArchetype::Grazer   ? makeGrazerMesh()
          : sp.archetype == EcoArchetype::Predator ? makePredatorMesh()
                                                   : makePatrolMesh();
        m_speciesMesh[si] = device.createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                              pm.index.data(), (uint32_t)pm.index.size());

        // Resolve the patrol route: authored waypoints, else a generated square
        // around the region center.
        if (sp.archetype == EcoArchetype::Patrol) {
            if (sp.waypoints.size() >= 4) {
                m_routes[si] = sp.waypoints;
            } else {
                const float r = sp.radius * 0.7f;
                m_routes[si] = { sp.regionX - r, sp.regionZ - r,
                                 sp.regionX + r, sp.regionZ - r,
                                 sp.regionX + r, sp.regionZ + r,
                                 sp.regionX - r, sp.regionZ + r };
            }
        }

        // Seed the agents.
        for (uint32_t k = 0; k < sp.count; ++k) {
            EcoAgent a;
            a.species = si;
            if (sp.archetype == EcoArchetype::Patrol && !m_routes[si].empty()) {
                // Stagger the squad along the route.
                const uint32_t nwp = (uint32_t)(m_routes[si].size() / 2);
                a.waypoint = (k * nwp) / std::max(1u, sp.count) % nwp;
                a.pos.x = m_routes[si][a.waypoint * 2 + 0];
                a.pos.z = m_routes[si][a.waypoint * 2 + 1];
                a.state = EcoState::PatrolMove;
            } else {
                // Cluster grazers near the region center; scatter predators wider.
                const float spread = (sp.archetype == EcoArchetype::Grazer)
                                         ? m_cfg.cohesionRadius : sp.radius * 0.6f;
                const float ang = (float)(rng() % 6283) * 0.001f;
                const float rad = (float)(rng() % 1000) * 0.001f * spread;
                a.pos.x = sp.regionX + std::cos(ang) * rad;
                a.pos.z = sp.regionZ + std::sin(ang) * rad;
                a.state = (sp.archetype == EcoArchetype::Grazer) ? EcoState::Graze
                                                                 : EcoState::Prowl;
                a.timer = m_cfg.grazeTimeMin
                        + (float)(rng() % 1000) * 0.001f
                              * (m_cfg.grazeTimeMax - m_cfg.grazeTimeMin);
            }
            a.pos.y   = groundAt(a.pos.x, a.pos.z);
            a.target  = a.pos;
            a.yaw     = (float)(rng() % 6283) * 0.001f;
            a.tintJitter = 0.85f + (float)(rng() % 1000) * 0.0003f;   // 0.85..1.15
            a.active  = false;

            Entity e;
            e.mesh = m_speciesMesh[si];
            e.baseColor[0] = sp.color[0] * a.tintJitter;
            e.baseColor[1] = sp.color[1] * a.tintJitter;
            e.baseColor[2] = sp.color[2] * a.tintJitter;
            e.baseColor[3] = 1.0f;
            e.visible = false;   // inactive until the player is inside the soft radius
            a.entity = scene.add(e);

            writeTransform(a, scene);
            m_agents.push_back(a);
        }
    }
    m_built = true;
    x3::logInfo("ecology: built " + std::to_string(m_agents.size()) + " agents across "
                + std::to_string(ns) + " species");
}

uint32_t AmbientEcology::activeCount() const {
    uint32_t n = 0;
    for (const EcoAgent& a : m_agents) if (a.active) ++n;
    return n;
}

uint32_t AmbientEcology::countInState(EcoState s) const {
    uint32_t n = 0;
    for (const EcoAgent& a : m_agents) if (a.state == s) ++n;
    return n;
}

bool AmbientEcology::herdCentroid(uint32_t species, float& outX, float& outZ) const {
    if (species >= m_centroidN.size() || m_centroidN[species] == 0) return false;
    outX = m_centroidX[species];
    outZ = m_centroidZ[species];
    return true;
}

void AmbientEcology::debugPlaceAgent(uint32_t i, const x3::phys::Vec3& p) {
    if (i >= m_agents.size()) return;
    m_agents[i].pos = p;
    m_agents[i].pos.y = groundAt(p.x, p.z);
    m_agents[i].target = m_agents[i].pos;
}

void AmbientEcology::commandInvestigate(const x3::phys::Vec3& pos) {
    for (EcoAgent& a : m_agents) {
        if (m_cfg.species[a.species].archetype != EcoArchetype::Patrol) continue;
        if (!a.onDuty) continue;
        a.state = EcoState::Investigate;
        a.stateTime = 0.0f;
        a.timer = 0.0f;
        a.targetAgent = 0u;   // patrol reuse: 0 = traveling, 1 = dwelling
        a.target = pos;
        a.target.y = groundAt(pos.x, pos.z);
    }
}

// ---------------------------------------------------------------------------
// decide() — the round-robin slice: activation hysteresis + state transitions.
// ---------------------------------------------------------------------------
void AmbientEcology::decide(uint32_t i, const x3::phys::Vec3& playerPos) {
    EcoAgent& a = m_agents[i];
    const EcoSpecies& sp = m_cfg.species[a.species];

    // ---- Soft-radius activation (hysteresis between the two radii) ----
    const float dp2 = dist2XZ(a.pos, playerPos);
    if (!a.active) {
        if (dp2 <= m_cfg.activeRadius * m_cfg.activeRadius) a.active = true;
        else return;   // inactive: zero sim
    } else if (dp2 >= m_cfg.despawnRadius * m_cfg.despawnRadius) {
        a.active = false;
        return;
    }

    switch (sp.archetype) {
    // =========================== GRAZER ===================================
    case EcoArchetype::Grazer: {
        if (a.state == EcoState::Down) {
            if (a.timer <= 0.0f) {
                // Respawn at the region edge, back to grazing.
                const float ang = (float)(rng() % 6283) * 0.001f;
                a.pos.x = sp.regionX + std::cos(ang) * sp.radius * 0.9f;
                a.pos.z = sp.regionZ + std::sin(ang) * sp.radius * 0.9f;
                a.pos.y = groundAt(a.pos.x, a.pos.z);
                a.state = EcoState::Graze;
                a.timer = m_cfg.grazeTimeMin;
            }
            return;
        }

        // Threat scan: nearest predator in fleeRadius, or the player too close.
        float threatX = 0, threatZ = 0;
        bool threatened = false;
        if (dp2 <= m_cfg.playerFleeRadius * m_cfg.playerFleeRadius) {
            threatX = playerPos.x; threatZ = playerPos.z; threatened = true;
        }
        if (!threatened) {
            const float fr2 = m_cfg.fleeRadius * m_cfg.fleeRadius;
            for (const EcoAgent& o : m_agents) {
                if (m_cfg.species[o.species].archetype != EcoArchetype::Predator) continue;
                if (!o.active || o.state == EcoState::Down) continue;
                if (dist2XZ(a.pos, o.pos) <= fr2) {
                    threatX = o.pos.x; threatZ = o.pos.z; threatened = true;
                    break;
                }
            }
        }
        if (threatened) {
            float ax = a.pos.x - threatX, az = a.pos.z - threatZ;
            const float len = std::sqrt(ax * ax + az * az);
            if (len > 0.01f) { ax /= len; az /= len; }
            else             { ax = 1.0f; az = 0.0f; }
            a.target.x = a.pos.x + ax * (m_cfg.fleeRadius + 8.0f);
            a.target.z = a.pos.z + az * (m_cfg.fleeRadius + 8.0f);
            a.state = EcoState::Flee;
            a.timer = m_cfg.fleeTime;   // refreshed while threatened
            return;
        }
        if (a.state == EcoState::Flee) {
            if (a.timer > 0.0f) return;          // keep running the burst out
            a.state = EcoState::Graze;
            a.timer = m_cfg.grazeTimeMin;
            return;
        }

        // Cohesion: too far from the herd centroid -> drift back.
        const float cx = (m_centroidN[a.species] > 0) ? m_centroidX[a.species] : sp.regionX;
        const float cz = (m_centroidN[a.species] > 0) ? m_centroidZ[a.species] : sp.regionZ;
        const float dcx = a.pos.x - cx, dcz = a.pos.z - cz;
        const float dc2 = dcx * dcx + dcz * dcz;
        if (dc2 > m_cfg.cohesionRadius * m_cfg.cohesionRadius) {
            a.target.x = cx; a.target.z = cz;
            a.state = EcoState::Wander;
            return;
        }
        // Region fence: drifted out of the home region -> walk back in.
        if (dist2XZ(a.pos, x3::phys::Vec3{sp.regionX, 0, sp.regionZ})
                > sp.radius * sp.radius) {
            a.target.x = sp.regionX; a.target.z = sp.regionZ;
            a.state = EcoState::Wander;
            return;
        }
        // Separation: shoulder-to-shoulder with a herdmate -> push apart a step.
        for (const EcoAgent& o : m_agents) {
            if (&o == &a || o.species != a.species || o.state == EcoState::Down) continue;
            if (dist2XZ(a.pos, o.pos) < m_cfg.separation * m_cfg.separation) {
                float ax = a.pos.x - o.pos.x, az = a.pos.z - o.pos.z;
                const float len = std::sqrt(ax * ax + az * az);
                if (len > 0.01f) { ax /= len; az /= len; } else { ax = 1; az = 0; }
                a.target.x = a.pos.x + ax * m_cfg.separation * 2.0f;
                a.target.z = a.pos.z + az * m_cfg.separation * 2.0f;
                a.state = EcoState::Wander;
                return;
            }
        }
        if (a.state == EcoState::Graze) {
            if (a.timer <= 0.0f) {
                // Drift to a fresh patch inside the cohesion envelope.
                const float ang = (float)(rng() % 6283) * 0.001f;
                const float rad = (float)(rng() % 1000) * 0.001f * m_cfg.cohesionRadius;
                a.target.x = cx + std::cos(ang) * rad;
                a.target.z = cz + std::sin(ang) * rad;
                a.state = EcoState::Wander;
            }
        } else if (a.state == EcoState::Wander) {
            if (dist2XZ(a.pos, a.target) < 1.0f) {
                a.state = EcoState::Graze;
                a.timer = m_cfg.grazeTimeMin
                        + (float)(rng() % 1000) * 0.001f
                              * (m_cfg.grazeTimeMax - m_cfg.grazeTimeMin);
            }
        } else {
            a.state = EcoState::Graze;
            a.timer = m_cfg.grazeTimeMin;
        }
        break;
    }
    // ========================== PREDATOR ==================================
    case EcoArchetype::Predator: {
        const x3::phys::Vec3 home{sp.regionX, 0, sp.regionZ};

        auto preyValid = [&](uint32_t idx) {
            if (idx >= m_agents.size()) return false;
            const EcoAgent& o = m_agents[idx];
            return m_cfg.species[o.species].archetype == EcoArchetype::Grazer
                && o.active && o.state != EcoState::Down;
        };

        if (a.state == EcoState::Feed) {
            if (a.timer <= 0.0f) { a.state = EcoState::Prowl; a.target = a.pos; }
            return;
        }

        if (a.state == EcoState::Stalk || a.state == EcoState::Strike) {
            // Track the live prey / player; abandon a chase that left the territory.
            if (dist2XZ(a.pos, home) > m_cfg.territoryRadius * m_cfg.territoryRadius) {
                a.state = EcoState::Prowl;
                a.target = home;
                a.targetAgent = 0xFFFFFFFFu;
                a.targetIsPlayer = false;
                return;
            }
            float d2 = 0.0f;
            if (a.targetIsPlayer) {
                a.target = playerPos;
                d2 = dp2;
                if (dp2 > m_cfg.stalkRadius * m_cfg.stalkRadius) {   // player escaped
                    a.state = EcoState::Prowl; a.targetIsPlayer = false;
                    return;
                }
            } else if (preyValid(a.targetAgent)) {
                a.target = m_agents[a.targetAgent].pos;
                d2 = dist2XZ(a.pos, a.target);
                if (d2 > m_cfg.stalkRadius * m_cfg.stalkRadius * 1.7f) {   // escaped
                    a.state = EcoState::Prowl; a.targetAgent = 0xFFFFFFFFu;
                    return;
                }
            } else {
                a.state = EcoState::Prowl;
                a.targetAgent = 0xFFFFFFFFu;
                a.targetIsPlayer = false;
                return;
            }
            a.state = (d2 <= m_cfg.strikeRadius * m_cfg.strikeRadius)
                          ? EcoState::Strike : EcoState::Stalk;
            return;
        }

        // Prowl: opportunistic player aggro first, then scan for grazer prey.
        if (dp2 <= m_cfg.playerAggroRadius * m_cfg.playerAggroRadius) {
            a.state = EcoState::Stalk;
            a.targetIsPlayer = true;
            a.target = playerPos;
            return;
        }
        {
            uint32_t best = 0xFFFFFFFFu;
            float bestD2 = m_cfg.stalkRadius * m_cfg.stalkRadius;
            for (uint32_t k = 0; k < (uint32_t)m_agents.size(); ++k) {
                if (!preyValid(k)) continue;
                const float d2 = dist2XZ(a.pos, m_agents[k].pos);
                if (d2 < bestD2) { bestD2 = d2; best = k; }
            }
            if (best != 0xFFFFFFFFu) {
                a.state = EcoState::Stalk;
                a.targetAgent = best;
                a.targetIsPlayer = false;
                a.target = m_agents[best].pos;
                return;
            }
        }
        // Cruise the territory.
        if (a.state != EcoState::Prowl) { a.state = EcoState::Prowl; a.target = a.pos; }
        if (dist2XZ(a.pos, a.target) < 1.5f) {
            const float ang = (float)(rng() % 6283) * 0.001f;
            const float rad = (float)(rng() % 1000) * 0.001f * sp.radius;
            a.target.x = sp.regionX + std::cos(ang) * rad;
            a.target.z = sp.regionZ + std::sin(ang) * rad;
        }
        break;
    }
    // =========================== PATROL ===================================
    case EcoArchetype::Patrol: {
        const std::vector<float>& route = m_routes[a.species];
        const uint32_t nwp = (uint32_t)(route.size() / 2);

        if (!a.onDuty) {
            // Stand down at the home post (region center).
            if (a.state != EcoState::OffDuty) {
                a.state = EcoState::OffDuty;
                a.target.x = sp.regionX; a.target.z = sp.regionZ;
            }
            return;
        }
        if (a.state == EcoState::Investigate) {
            // Dwell latch lives in integrate() (arrival sets targetAgent=1 + the
            // timer); once the look-around dwell expires, resume the route.
            if (a.targetAgent == 1u && a.timer <= 0.0f) {
                a.state = EcoState::PatrolMove;
                a.targetAgent = 0xFFFFFFFFu;
            }
            return;
        }
        if (nwp == 0) { a.state = EcoState::PatrolHold; return; }
        if (a.state == EcoState::OffDuty) a.state = EcoState::PatrolMove;   // back on shift

        if (a.state == EcoState::PatrolHold) {
            if (a.timer <= 0.0f) {
                a.waypoint = (a.waypoint + 1) % nwp;
                a.state = EcoState::PatrolMove;
            }
            return;
        }
        // PatrolMove: walk the current waypoint; arrive -> hold.
        a.target.x = route[a.waypoint * 2 + 0];
        a.target.z = route[a.waypoint * 2 + 1];
        if (a.state != EcoState::PatrolMove) a.state = EcoState::PatrolMove;
        if (dist2XZ(a.pos, a.target) < m_cfg.waypointArrive * m_cfg.waypointArrive) {
            a.state = EcoState::PatrolHold;
            a.timer = m_cfg.holdTime;
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// integrate() — every-frame, every-agent: visibility sync, timers, dt-scaled
// movement toward target, kill-contact for striking predators.
// ---------------------------------------------------------------------------
void AmbientEcology::integrate(uint32_t i, float dt, Scene& scene) {
    EcoAgent& a = m_agents[i];
    const EcoSpecies& sp = m_cfg.species[a.species];

    // Visibility sync (cheap branch; hidden when inactive or Down).
    if (a.entity != kNoLink) {
        Entity& e = scene.get(a.entity);
        const bool wantVis = a.active && a.state != EcoState::Down;
        if (e.visible != wantVis) e.visible = wantVis;
    }
    if (!a.active) return;   // zero sim cost beyond the visibility branch

    a.stateTime += dt;
    if (a.timer > 0.0f) a.timer -= dt;

    // Investigate dwell: arrival flips the traveling latch and starts the
    // look-around timer (decide() resumes the route when it expires).
    if (a.state == EcoState::Investigate && a.targetAgent == 0u
        && dist2XZ(a.pos, a.target) < m_cfg.waypointArrive * m_cfg.waypointArrive) {
        a.targetAgent = 1u;
        a.timer = m_cfg.investigateTime;
    }

    // Per-state speed.
    float speed = 0.0f;
    switch (a.state) {
        case EcoState::Wander:      speed = sp.speed; break;
        case EcoState::Flee:        speed = sp.fastSpeed; break;
        case EcoState::Prowl:       speed = sp.speed; break;
        case EcoState::Stalk:       speed = sp.fastSpeed; break;
        case EcoState::Strike:      speed = sp.fastSpeed * m_cfg.strikeSpeedMul; break;
        case EcoState::PatrolMove:  speed = sp.speed * m_patrolSpeedMul; break;
        case EcoState::Investigate: speed = sp.fastSpeed * m_patrolSpeedMul; break;
        case EcoState::OffDuty:     speed = sp.speed; break;
        default: break;   // Graze / Feed / Down / PatrolHold: stand
    }

    if (speed > 0.0f) {
        const float dx = a.target.x - a.pos.x, dz = a.target.z - a.pos.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len > 0.05f) {
            const float step = std::min(speed * dt, len);
            a.pos.x += dx / len * step;
            a.pos.z += dz / len * step;
            a.pos.y = groundAt(a.pos.x, a.pos.z);
            // Face the motion (-Z forward convention): yaw = atan2(-dx, -dz).
            a.yaw = slewYaw(a.yaw, std::atan2(-dx, -dz), 8.0f * dt);
        }
    }

    // Strike contact: the lunge connects -> prey goes Down, the herd panics.
    if (a.state == EcoState::Strike && !a.targetIsPlayer
        && a.targetAgent < (uint32_t)m_agents.size()) {
        EcoAgent& prey = m_agents[a.targetAgent];
        if (prey.state != EcoState::Down
            && dist2XZ(a.pos, prey.pos) <= m_cfg.killRadius * m_cfg.killRadius) {
            prey.state = EcoState::Down;
            prey.timer = m_cfg.downRespawnTime;
            a.state = EcoState::Feed;
            a.timer = m_cfg.feedTime;
            a.targetAgent = 0xFFFFFFFFu;
            // Panic every herdmate within the panic radius.
            const float pr2 = m_cfg.panicRadius * m_cfg.panicRadius;
            for (EcoAgent& o : m_agents) {
                if (o.species != prey.species || &o == &prey) continue;
                if (o.state == EcoState::Down) continue;
                if (dist2XZ(o.pos, prey.pos) > pr2) continue;
                float ax = o.pos.x - prey.pos.x, az = o.pos.z - prey.pos.z;
                const float l = std::sqrt(ax * ax + az * az);
                if (l > 0.01f) { ax /= l; az /= l; } else { ax = 1; az = 0; }
                o.state = EcoState::Flee;
                o.timer = m_cfg.fleeTime;
                o.target.x = o.pos.x + ax * (m_cfg.fleeRadius + 10.0f);
                o.target.z = o.pos.z + az * (m_cfg.fleeRadius + 10.0f);
            }
        }
    }

    writeTransform(a, scene);
}

void AmbientEcology::update(float dt, Scene& scene, const x3::phys::Vec3& playerPos,
                            TodPhase phase) {
    if (!m_built || m_agents.empty()) return;

    // ---- Schedule: resolve the shift; on a change, flip patrol duty flags ----
    const bool day = (phase == TodPhase::Dawn || phase == TodPhase::Day);
    if (!m_shiftInit || day != m_dayShift) {
        m_dayShift = day;
        for (EcoAgent& a : m_agents) {
            const EcoSpecies& sp = m_cfg.species[a.species];
            if (sp.archetype != EcoArchetype::Patrol) continue;
            const bool duty = sp.schedule == EcoSchedule::Always
                           || (sp.schedule == EcoSchedule::Day && day)
                           || (sp.schedule == EcoSchedule::Night && !day);
            a.onDuty = duty;
            a.state  = duty ? EcoState::PatrolMove : EcoState::OffDuty;
            if (!duty) { a.target.x = sp.regionX; a.target.z = sp.regionZ; }
        }
        if (m_shiftInit)
            x3::logInfo(std::string("ecology: shift change -> ")
                        + (day ? "DAY" : "NIGHT") + " patrol on duty");
        m_shiftInit = true;
    }

    // ---- Herd centroids (one pass; reused by every grazer decision) ----
    std::fill(m_centroidX.begin(), m_centroidX.end(), 0.0f);
    std::fill(m_centroidZ.begin(), m_centroidZ.end(), 0.0f);
    std::fill(m_centroidN.begin(), m_centroidN.end(), 0u);
    for (const EcoAgent& a : m_agents) {
        if (m_cfg.species[a.species].archetype != EcoArchetype::Grazer) continue;
        if (a.state == EcoState::Down) continue;
        m_centroidX[a.species] += a.pos.x;
        m_centroidZ[a.species] += a.pos.z;
        m_centroidN[a.species] += 1;
    }
    for (size_t s = 0; s < m_centroidN.size(); ++s) {
        if (m_centroidN[s] == 0) continue;
        m_centroidX[s] /= (float)m_centroidN[s];
        m_centroidZ[s] /= (float)m_centroidN[s];
    }

    // ---- Round-robin decisions (budgeted) ----
    const uint32_t n = (uint32_t)m_agents.size();
    const uint32_t budget = std::min(m_cfg.decisionsPerFrame, n);
    for (uint32_t k = 0; k < budget; ++k) {
        decide(m_cursor % n, playerPos);
        m_cursor = (m_cursor + 1) % n;
    }

    // ---- Cheap dt-scaled integration for every agent ----
    for (uint32_t i = 0; i < n; ++i) integrate(i, dt, scene);
}

// ===========================================================================
// Headless self-test (--test-ecology)
// ===========================================================================

namespace {

int t_pass = 0, t_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++t_pass; x3::logInfo(std::string("[ecology-test] PASS ") + name); }
    else      { ++t_fail; x3::logError(std::string("[ecology-test] FAIL ") + name); }
}

// Headless device that counts createMesh calls (leak canary for T7).
class CountingDevice final : public HeadlessRenderDevice {
public:
    uint32_t meshCreates = 0;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t nv,
                                   const uint32_t* idx, uint32_t ni) override {
        ++meshCreates;
        return HeadlessRenderDevice::createMesh(v, nv, idx, ni);
    }
};

} // namespace

bool runEcologySelfTest() {
    t_pass = t_fail = 0;
    CountingDevice device;
    Scene scene;
    AmbientEcology eco;
    const EcoConfig cfg = defaultEcoConfig();
    eco.build(cfg, scene, device);

    const float dt = 1.0f / 60.0f;
    auto tick = [&](const x3::phys::Vec3& player, TodPhase phase, int frames) {
        for (int f = 0; f < frames; ++f) eco.update(dt, scene, player, phase);
    };

    // Indices of the default cast: grazers are agents [0..9], predators [10..11],
    // day patrol [12..14], night patrol [15..17].
    uint32_t total = 0;
    for (const EcoSpecies& s : cfg.species) total += s.count;

    // ---- T1: build counts match the config; one shared mesh per species ----
    check(eco.agentCount() == total && scene.size() == total
              && device.meshCreates == (uint32_t)cfg.species.size(),
          "T1 build counts (agents/entities/shared species meshes)");

    // Player parked NEAR the herd (inside the soft radius so everything around
    // it simulates, but outside playerFleeRadius so it doesn't spook the herd).
    const x3::phys::Vec3 nearHerd{30.0f, 0.0f, 40.0f};

    // Park both predators far outside the soft radius (inactive = no threat) so
    // T2 measures SETTLED cohesion, not a herd mid-hunt. T3/T8 stage them back.
    eco.debugPlaceAgent(10, x3::phys::Vec3{500.0f, 0.0f, 500.0f});
    eco.debugPlaceAgent(11, x3::phys::Vec3{520.0f, 0.0f, 500.0f});

    // ---- T2: cohesion bounds — settled herd stays in the envelope + region ----
    tick(nearHerd, TodPhase::Day, 60 * 30);   // 30 s settle
    {
        float cx = 0, cz = 0;
        bool haveC = eco.herdCentroid(0, cx, cz);
        bool inEnvelope = haveC;
        const float bound = cfg.cohesionRadius * 2.0f;   // slack for in-flight wanders
        const float region2 = (cfg.species[0].radius * 1.2f) * (cfg.species[0].radius * 1.2f);
        for (uint32_t i = 0; i < 10; ++i) {
            const EcoAgent& a = eco.agent(i);
            if (a.state == EcoState::Down) continue;
            const float dx = a.pos.x - cx, dz = a.pos.z - cz;
            if (dx * dx + dz * dz > bound * bound) inEnvelope = false;
            const float rx = a.pos.x - cfg.species[0].regionX,
                        rz = a.pos.z - cfg.species[0].regionZ;
            if (rx * rx + rz * rz > region2) inEnvelope = false;
        }
        check(inEnvelope, "T2 herd cohesion bounds (envelope + region fence)");
    }

    // ---- T3: flee trigger — a staged predator flips a grazer to Flee, moving AWAY ----
    {
        const EcoAgent& g = eco.agent(0);
        x3::phys::Vec3 stage = g.pos; stage.x += 2.0f;     // predator right beside it
        eco.debugPlaceAgent(10, stage);
        const float beforeX = g.pos.x;
        tick(nearHerd, TodPhase::Day, 90);                  // 1.5 s: decide + run
        const bool fled = (g.state == EcoState::Flee || g.state == EcoState::Down
                           || eco.countInState(EcoState::Flee) > 0);
        // Fled AWAY: the predator was at +X of the grazer, so it should move -X.
        check(fled && (g.state == EcoState::Down || g.pos.x <= beforeX + 0.5f),
              "T3 flee trigger (predator proximity -> Flee, away from threat)");
    }
    tick(nearHerd, TodPhase::Day, 60 * 10);   // settle the staged drama back down

    // ---- T4: patrol waypoint adherence (on-duty day patrol walks its corners) ----
    {
        // Park the player at the patrol region so the squad is active.
        const x3::phys::Vec3 nearPatrol{cfg.species[2].regionX, 0.0f, cfg.species[2].regionZ};
        const EcoAgent& p = eco.agent(12);
        uint32_t visited = 0;
        uint32_t lastWp = p.waypoint;
        bool stayedOnRoute = true;
        const float fence2 = (cfg.species[2].radius * 1.6f) * (cfg.species[2].radius * 1.6f);
        for (int f = 0; f < 60 * 120 && visited < 4; ++f) {   // up to 2 min sim
            eco.update(dt, scene, nearPatrol, TodPhase::Day);
            if (p.waypoint != lastWp) { ++visited; lastWp = p.waypoint; }
            const float rx = p.pos.x - cfg.species[2].regionX,
                        rz = p.pos.z - cfg.species[2].regionZ;
            if (rx * rx + rz * rz > fence2) stayedOnRoute = false;
        }
        check(p.onDuty && visited >= 4 && stayedOnRoute,
              "T4 patrol waypoint adherence (visits corners in order, stays on route)");
    }

    // ---- T5: schedule switch — Day -> Night swaps the on-duty patrol shift ----
    {
        const x3::phys::Vec3 nearPatrol{cfg.species[2].regionX, 0.0f, cfg.species[2].regionZ};
        eco.update(dt, scene, nearPatrol, TodPhase::Day);
        const bool dayDuty = eco.agent(12).onDuty && !eco.agent(15).onDuty
                          && eco.isDayShift();
        eco.update(dt, scene, nearPatrol, TodPhase::Night);
        const bool nightDuty = !eco.agent(12).onDuty && eco.agent(15).onDuty
                            && !eco.isDayShift()
                            && eco.agent(12).state == EcoState::OffDuty
                            && eco.agent(15).state == EcoState::PatrolMove;
        check(dayDuty && nightDuty, "T5 schedule switch (Day/Night shift swap)");
    }

    // ---- T6: distance spawn/despawn (activation hysteresis) ----
    {
        tick(nearHerd, TodPhase::Day, 10);
        const uint32_t activeNear = eco.activeCount();
        const x3::phys::Vec3 farAway{5000.0f, 0.0f, 5000.0f};
        tick(farAway, TodPhase::Day, 10);   // round-robin sweep deactivates all
        const uint32_t activeFar = eco.activeCount();
        bool hidden = true;
        for (uint32_t i = 0; i < eco.agentCount(); ++i)
            if (scene.get(eco.agent(i).entity).visible) hidden = false;
        check(activeNear > 0 && activeFar == 0 && hidden,
              "T6 distance spawn/despawn (active near, deactivated+hidden far)");
    }

    // ---- T7: leak/budget — long ticking creates NO meshes/entities ----
    {
        const uint32_t meshesBefore = device.meshCreates;
        const uint32_t entsBefore   = scene.size();
        const uint32_t agentsBefore = eco.agentCount();
        for (int f = 0; f < 60 * 60; ++f) {   // 1 min, player orbiting the world
            const float ang = (float)f * 0.002f;
            const x3::phys::Vec3 p{30.0f + std::cos(ang) * 80.0f, 0.0f,
                                   std::sin(ang) * 80.0f};
            eco.update(dt, scene, p, (f % 2000 < 1000) ? TodPhase::Day : TodPhase::Night);
        }
        check(device.meshCreates == meshesBefore && scene.size() == entsBefore
                  && eco.agentCount() == agentsBefore,
              "T7 leak/budget (no new meshes/entities across long ticking)");
    }

    // ---- T8: the predator-strike moment — a kill scatters the herd ----
    {
        // Observe from OFF to the side: inside the soft radius (everything still
        // simulates) but beyond playerAggroRadius, so the staged stalker hunts the
        // HERD instead of opportunistically posturing at the player.
        const x3::phys::Vec3 observer{30.0f, 0.0f, 40.0f};
        eco.debugPlaceAgent(10, x3::phys::Vec3{500.0f, 0.0f, 500.0f});   // re-park
        // Deterministic staging: clump the herd in a tight ring at the region
        // center (earlier tests may have scattered it / left edge-respawners).
        for (uint32_t i = 0; i < 10; ++i) {
            const float ang = (float)i * 0.628f;
            eco.debugPlaceAgent(i, x3::phys::Vec3{30.0f + std::cos(ang) * 4.0f, 0.0f,
                                                  std::sin(ang) * 4.0f});
        }
        tick(observer, TodPhase::Day, 60 * 2);   // let the clump settle into grazing
        float cx = 30.0f, cz = 0.0f;
        eco.herdCentroid(0, cx, cz);
        eco.debugPlaceAgent(10, x3::phys::Vec3{cx + 6.0f, 0.0f, cz});   // stage the stalker
        bool downed = false;
        uint32_t fleeing = 0, herdLive = 0;
        for (int f = 0; f < 60 * 30; ++f) {
            eco.update(dt, scene, observer, TodPhase::Day);
            if (eco.countInState(EcoState::Down) > 0) {
                downed = true;
                fleeing = eco.countInState(EcoState::Flee);
                herdLive = 0;
                for (uint32_t i = 0; i < 10; ++i)
                    if (eco.agent(i).state != EcoState::Down) ++herdLive;
                if (fleeing * 2 >= herdLive) break;   // scatter observed
            }
        }
        check(downed && fleeing * 2 >= herdLive,
              "T8 predator strike (kill lands, herd scatters >= half fleeing)");
    }

    x3::logInfo("ecology: " + std::to_string(t_pass) + "/"
                + std::to_string(t_pass + t_fail) + " passed");
    return t_fail == 0;
}

} // namespace x3::game
