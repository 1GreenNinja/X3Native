#pragma once
// AMBIENT ECOLOGY — the Keth'zar surface LIVES (living-world pillar 1).
// Game/slice code only — engine/ stays pure.
//
// A lightweight ambient-agent layer that sits BESIDE the combat MonsterSystem
// (it does not touch it): herd GRAZERS that wander/graze and flee from players
// and predators with flocking cohesion, PREDATORS that prowl a territory, stalk
// the herds, strike, and opportunistically posture at the player, and facility
// PATROLS that walk waypoint routes with DAY/NIGHT shift changes driven by the
// existing Time-of-Day cycle (tod.h — the skyTime schedule hook).
//
// DESIGN STANCE (the streaming-budget spirit):
//   * Agents are PURE KINEMATIC SIMULACRA — no physics bodies, no skinning, no
//     per-agent model loads. Each species shares ONE procedural compound mesh
//     (a readable boxy critter, built once at build()); each agent is a Scene
//     entity whose transform is rewritten as it moves. Cost is a few hundred
//     floats per frame, flat.
//   * SOFT-RADIUS ACTIVATION: an agent is ACTIVE (simulated + drawn) only while
//     the player is within cfg.activeRadius of it; beyond cfg.despawnRadius it
//     DEACTIVATES (entity hidden, zero sim cost). Hysteresis between the two
//     radii prevents popping at the boundary.
//   * FRAME BUDGET: per-frame DECISION work (state changes, threat scans, the
//     activation distance check) is ROUND-ROBIN across cfg.decisionsPerFrame
//     agents; only cheap dt-scaled movement integration runs for every active
//     agent every frame. No per-frame heap allocation in update().
//   * DATA-DRIVEN: the whole cast (species -> behavior archetype, counts,
//     region, schedule, speeds, colors, patrol waypoints) loads from
//     assets/world/ecology.json (loadEcologyConfig). A missing/broken JSON
//     falls back to the built-in default cast so a clean checkout never breaks.
//
// The behaviors are deliberately READABLE AT A GLANCE: a grazer herd grazing in
// a loose clump, scattering as one when a predator strikes, a patrol filing
// between waypoints and trading shifts at dusk — that IS the demo.

#include "scene.h"
#include "tod.h"   // TodPhase — the day/night schedule hook

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// Behavior archetype a species runs. Decides which state machine drives it.
enum class EcoArchetype : uint32_t { Grazer = 0, Predator = 1, Patrol = 2 };
const char* ecoArchetypeName(EcoArchetype a);

// When a PATROL species is ON DUTY (grazers/predators are wildlife: Always).
enum class EcoSchedule : uint32_t { Always = 0, Day = 1, Night = 2 };
const char* ecoScheduleName(EcoSchedule s);

// Per-agent behavior state. Which subset applies depends on the archetype.
enum class EcoState : uint32_t {
    Graze = 0,      // grazer: head down, idling in the clump
    Wander,         // grazer: drifting (toward the herd / a fresh patch)
    Flee,           // grazer: sprinting AWAY from a threat (predator/player)
    Prowl,          // predator: cruising its territory, scanning
    Stalk,          // predator: locked onto a grazer (or the player), closing
    Strike,         // predator: the lunge
    Feed,           // predator: holding over a downed kill
    Down,           // grazer: taken by a predator (hidden; respawns later)
    PatrolMove,     // patrol: walking its waypoint route (on duty)
    PatrolHold,     // patrol: holding at a waypoint / standing post
    Investigate,    // patrol: commanded to a stimulus (alert hook)
    OffDuty,        // patrol: schedule says stand down (parked at home point)
};
const char* ecoStateName(EcoState s);

// One species row of the ecology cast (a parsed ecology.json "species" entry).
struct EcoSpecies {
    std::string  name      = "Critter";
    EcoArchetype archetype = EcoArchetype::Grazer;
    EcoSchedule  schedule  = EcoSchedule::Always;   // patrols only (wildlife: Always)
    uint32_t     count     = 4;       // how many agents to spawn
    float        regionX   = 0.0f;    // home-region center (world XZ)
    float        regionZ   = 0.0f;
    float        radius    = 50.0f;   // home-region radius (m): agents stay inside
    float        speed     = 1.6f;    // cruise speed (m/s): graze-wander / prowl / patrol
    float        fastSpeed = 5.0f;    // flee (grazer) / stalk-strike base (predator)
    float        scale     = 1.0f;    // visual scale of the shared species mesh
    float        color[3]  = { 0.6f, 0.6f, 0.6f };  // base tint (slight per-agent variance)
    // Patrol-only: the waypoint route (world XZ pairs; Y is resolved via groundY).
    // Empty for a Patrol species => a generated square route around the region center.
    std::vector<float> waypoints;     // flat x,z pairs
};

// Whole-cast config + the global behavior tunables (the JSON top level).
struct EcoConfig {
    // ---- Soft-radius activation (the streaming-budget spirit) ----
    float activeRadius      = 140.0f;  // agent activates when player is within this (m)
    float despawnRadius     = 165.0f;  // agent deactivates beyond this (m; > activeRadius)
    // ---- Frame budget ----
    uint32_t decisionsPerFrame = 16;   // round-robin decision updates per frame
    // ---- Grazer behavior ----
    float cohesionRadius    = 14.0f;   // farther than this from the herd centroid -> drift back
    float separation        = 2.0f;    // closer than this to a herdmate -> push apart
    float fleeRadius        = 16.0f;   // predator inside this -> flee
    float playerFleeRadius  = 9.0f;    // player inside this -> flee
    float fleeTime          = 3.5f;    // seconds a flee burst lasts (refreshed while threatened)
    float panicRadius       = 22.0f;   // a STRIKE panics every herdmate within this
    float grazeTimeMin      = 2.0f;    // graze-idle dwell band (s)
    float grazeTimeMax      = 6.0f;
    // ---- Predator behavior ----
    float stalkRadius       = 45.0f;   // scan range for grazer prey
    float strikeRadius      = 9.0f;    // close enough -> lunge
    float killRadius        = 1.2f;    // lunge contact: the grazer goes Down
    float strikeSpeedMul    = 1.6f;    // strike speed = fastSpeed * this
    float feedTime          = 6.0f;    // seconds spent over the kill before prowling again
    float downRespawnTime   = 30.0f;   // a Down grazer respawns at the region edge after this
    float playerAggroRadius = 12.0f;   // player closer than this -> opportunistic stalk
    float territoryRadius   = 60.0f;   // predator abandons a player chase beyond this from home
    // ---- Patrol behavior ----
    float waypointArrive    = 1.2f;    // "reached" a waypoint within this (m)
    float holdTime          = 2.0f;    // dwell at each waypoint (s)
    float investigateTime   = 8.0f;    // look-around dwell at a commanded stimulus (s)
    float alertSpeedMul     = 1.6f;    // route speed multiplier while alerted (tighter routes)
    // ---- World hookup ----
    float groundY           = 0.0f;    // flat-ground fallback (overridable via setGroundFn)

    std::vector<EcoSpecies> species;   // the cast
};

// Built-in default cast (the same content as assets/world/ecology.json): a
// Shardhorn grazer herd, Crystal Stalker predators (bestiary Act-2 fauna), and
// Dominion day/night patrol shifts. Used when the JSON is absent/unreadable.
EcoConfig defaultEcoConfig();

// Load the cast + tunables from an ecology.json. Returns defaultEcoConfig() on
// a missing/unparseable file (logged; a clean checkout never breaks).
EcoConfig loadEcologyConfig(std::string_view jsonPath);

// Canonical on-disk path: <assetRoot>/world/ecology.json.
std::string ecologyJsonPath();

// One live ambient agent.
struct EcoAgent {
    uint32_t   species   = 0;            // index into config().species
    uint32_t   entity    = kNoLink;      // its Scene entity (shared species mesh)
    EcoState   state     = EcoState::Graze;
    x3::phys::Vec3 pos{};                // world position (feet)
    float      yaw       = 0.0f;         // facing (radians; -Z forward convention)
    float      stateTime = 0.0f;         // seconds in the current state
    float      timer     = 0.0f;         // state-specific countdown (graze dwell / flee / feed)
    x3::phys::Vec3 target{};             // move target (wander goal / stalk prey / waypoint)
    uint32_t   targetAgent = 0xFFFFFFFFu;// predator: stalked agent index (or none)
    bool       targetIsPlayer = false;   // predator: posture target is the player
    uint32_t   waypoint  = 0;            // patrol: current route index
    bool       active    = false;        // inside the soft radius: simulated + drawn
    bool       onDuty    = true;         // patrol: schedule says work this shift
    float      tintJitter = 0.0f;        // per-agent color variance (visual)
};

// The ambient-ecology layer. Build once into a Scene; update every frame with
// the player position + the current Time-of-Day phase.
class AmbientEcology {
public:
    // Build the cast into `scene`: one shared procedural mesh per species, one
    // Scene entity per agent, agents seeded around their region centers. No
    // physics bodies. Call once.
    void build(const EcoConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device);

    // Optional terrain hookup: agents ride groundFn(x,z) instead of the flat
    // cfg.groundY. Borrowed for the system's lifetime; may be empty (flat).
    void setGroundFn(std::function<float(float, float)> fn) { m_groundFn = std::move(fn); }

    // Advance one frame. `playerPos` drives activation, flee and predator
    // posturing; `phase` drives the patrol shift schedule (Dawn/Day = day shift,
    // Dusk/Night = night shift). Movement is dt-scaled; decisions round-robin.
    void update(float dt, Scene& scene, const x3::phys::Vec3& playerPos, TodPhase phase);

    // ---- Alert hooks (pillar 3 drives the facility patrols through these) ----
    // Route every ON-DUTY patrol agent (any species) to `pos` to look around.
    void commandInvestigate(const x3::phys::Vec3& pos);
    // Speed multiplier on patrol routes (1 = calm; alert levels tighten this).
    void setPatrolSpeedMul(float mul) { m_patrolSpeedMul = (mul < 0.1f) ? 0.1f : mul; }

    // ---- Queries (host HUD / self-test) ----
    bool     built() const { return m_built; }
    const EcoConfig& config() const { return m_cfg; }
    uint32_t agentCount() const { return (uint32_t)m_agents.size(); }
    const EcoAgent& agent(uint32_t i) const { return m_agents[i]; }
    uint32_t activeCount() const;                  // agents inside the soft radius
    uint32_t countInState(EcoState s) const;       // diagnostics / tests
    // Herd centroid of a grazer species (world XZ; valid if it has live agents).
    bool herdCentroid(uint32_t species, float& outX, float& outZ) const;
    // Current shift the schedule resolved to (after the last update()).
    bool isDayShift() const { return m_dayShift; }

    // ---- Debug/demo helpers ----
    // Teleport agent `i` (test: stage a predator on a herd; demo: a guaranteed moment).
    void debugPlaceAgent(uint32_t i, const x3::phys::Vec3& p);

private:
    void   decide(uint32_t i, const x3::phys::Vec3& playerPos);   // round-robin slice
    void   integrate(uint32_t i, float dt, Scene& scene);          // every active agent
    float  groundAt(float x, float z) const;
    void   writeTransform(EcoAgent& a, Scene& scene);
    uint32_t rng();                                                // tiny LCG

    bool        m_built = false;
    EcoConfig   m_cfg{};
    std::vector<EcoAgent> m_agents;
    std::vector<x3::rhi::MeshHandle> m_speciesMesh;   // one shared mesh per species
    std::vector<std::vector<float>>  m_routes;        // resolved patrol routes (x,z pairs)
    std::function<float(float, float)> m_groundFn;    // empty => flat cfg.groundY
    uint32_t    m_cursor = 0;          // round-robin decision cursor
    uint32_t    m_rngState = 0xB5297A4Du;
    bool        m_dayShift = true;     // resolved shift (from the last update's phase)
    bool        m_shiftInit = false;   // first update seeds the shift without a "change"
    float       m_patrolSpeedMul = 1.0f;
    // Per-species herd centroid scratch (recomputed once per update; no per-frame alloc).
    std::vector<float> m_centroidX, m_centroidZ;
    std::vector<uint32_t> m_centroidN;
};

// Headless self-test (--test-ecology). Builds the default cast on a counting
// HeadlessRenderDevice + Scene and asserts: (T1) build counts match the config;
// (T2) herd cohesion bounds — after settling, every live grazer stays within the
// cohesion envelope of its herd centroid and inside its region; (T3) flee
// triggers — a predator staged next to a grazer flips it to Flee, moving AWAY;
// (T4) patrol waypoint adherence — an on-duty patrol visits its route corners in
// order and never strays far from the route; (T5) schedule switch — flipping the
// TodPhase Day -> Night swaps which patrol shift is on duty; (T6) spawn/despawn
// by distance — agents activate near the player and deactivate (hidden, zero
// sim) when the player leaves; (T7) leak/budget — long ticking creates NO new
// meshes/entities and agent counts stay constant; (T8) the predator-strike
// moment — a staged stalker downs a grazer and the herd scatters (>= half the
// herd fleeing at once). Prints "ecology: X/Y passed"; returns true iff all
// pass. No window / Vulkan. Lives in ecology.cpp.
bool runEcologySelfTest();

} // namespace x3::game
