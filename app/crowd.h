#pragma once
// CROWDS — Club 1127 dancers + facility civilians (living-world pillar 2).
// Game/slice code only — engine/ stays pure.
//
// A lightweight ambient crowd layer in the same SIMULACRA stance as the ambient
// ecology (app/ecology.h): agents are pure kinematic Scene entities sharing ONE
// procedural humanoid compound mesh (per-agent palette tint + optional neon
// emissive for the club's blacklit dancers). No physics bodies, no skinning, no
// per-agent loads. Behaviors are readable at a glance:
//   * IDLE clusters — stand in loose knots; club crowds "dance" (a yaw sway +
//     a small bob driven by a per-agent phase).
//   * WANDER between points — drift between the configured hangout points
//     (dance floor, bars, couches / ward benches) on random dwell timers.
//   * SCATTER + COWER on violence — CrowdSystem::onViolence(pos) sends every
//     agent within scatterRadius sprinting AWAY from the violence (clamped to
//     the room region), where it then COWERS (crouched, trembling-still).
//   * RETURN after calm — once calmTime passes with no further violence, the
//     cowering crowd stands back up and wanders back to its points.
//
// Counts are small (a club floor is 10-20 NPCs), so every agent updates every
// frame with cheap dt-scaled math — no round-robin needed; still zero per-frame
// heap allocation. The host feeds onViolence() from its gunshot/melee/death
// events (Level 1 wires onFire; the club is peaceful unless the player swings).

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// Per-agent behavior state.
enum class CrowdState : uint32_t {
    Idle = 0,    // standing in the cluster (club: dancing in place)
    Wander,      // drifting toward a hangout point
    Scatter,     // sprinting away from a violence position
    Cower,       // huddled down after scattering (until calm)
};
const char* crowdStateName(CrowdState s);

// Crowd configuration (authored by the host area: club / facility floor).
struct CrowdConfig {
    uint32_t count   = 12;          // how many NPCs
    float    centerX = 0.0f;        // room/region center (world)
    float    centerZ = 0.0f;
    float    groundY = 0.0f;        // floor height the crowd stands on
    float    radius  = 10.0f;       // hard region clamp (agents never leave it)
    // Hangout points (world XZ pairs). Empty => a generated ring of 4 points
    // around the center at half the radius.
    std::vector<float> points;
    float    walkSpeed   = 1.2f;    // wander speed (m/s)
    float    fleeSpeed   = 4.5f;    // scatter sprint (m/s)
    float    scatterRadius = 18.0f; // violence within this scatters an agent
    float    calmTime    = 6.0f;    // seconds of quiet before the crowd returns
    float    dwellMin    = 2.0f;    // idle dwell band between wanders (s)
    float    dwellMax    = 7.0f;
    bool     dance       = false;   // club: idle agents sway/bob to the beat
    float    emissive    = 0.0f;    // >0: neon-tinted glow strength (club look)
    float    scale       = 1.0f;    // visual scale of the shared mesh
};

// One crowd NPC.
struct CrowdAgent {
    uint32_t   entity = kNoLink;
    CrowdState state  = CrowdState::Idle;
    x3::phys::Vec3 pos{};
    x3::phys::Vec3 target{};
    float      yaw    = 0.0f;
    float      timer  = 0.0f;       // dwell / scatter-burst countdown
    float      phase  = 0.0f;       // per-agent dance/tremble phase offset
};

// The crowd layer. Build once into a Scene; update every frame; feed violence.
class CrowdSystem {
public:
    // Build `cfg.count` NPCs sharing one procedural humanoid mesh, seeded in a
    // loose cluster around the hangout points. Call once.
    void build(const CrowdConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device);

    // Advance one frame (dt-scaled movement; dance sway; calm countdown).
    void update(float dt, Scene& scene);

    // Violence at `pos` (gunshot / melee / a death): every agent within
    // scatterRadius sprints away then cowers; resets the calm countdown.
    void onViolence(const x3::phys::Vec3& pos);

    // ---- Queries (host HUD / self-test) ----
    bool     built() const { return m_built; }
    const CrowdConfig& config() const { return m_cfg; }
    uint32_t agentCount() const { return (uint32_t)m_agents.size(); }
    const CrowdAgent& agent(uint32_t i) const { return m_agents[i]; }
    uint32_t countInState(CrowdState s) const;
    // True when no violence is pending (the calm countdown has expired).
    bool     calm() const { return m_calmTimer <= 0.0f; }

private:
    void writeTransform(CrowdAgent& a, Scene& scene, float bob, float crouch);
    void clampToRegion(float& x, float& z) const;
    uint32_t rng();

    bool        m_built = false;
    CrowdConfig m_cfg{};
    std::vector<CrowdAgent> m_agents;
    std::vector<float> m_points;     // resolved hangout points (x,z pairs)
    x3::rhi::MeshHandle m_mesh{};
    float       m_calmTimer = 0.0f;  // > 0 while violence is fresh
    float       m_time = 0.0f;       // dance clock
    uint32_t    m_rngState = 0x9E3779B9u;
};

// Headless self-test (--test-crowd). Asserts: (C1) build counts + one shared
// mesh; (C2) the idling/wandering crowd stays inside its region; (C3) violence
// scatters nearby agents AWAY from the point; (C4) scattered agents settle into
// Cower while violence is fresh; (C5) after calmTime of quiet the crowd returns
// to Idle/Wander; (C6) long ticking leaks no meshes/entities. Prints
// "crowd: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runCrowdSelfTest();

} // namespace x3::game
