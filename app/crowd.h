#pragma once
// CROWDS — Club 1127 dancers + facility civilians + LIVING NPCs (living-world
// pillar 2). Game/slice code only — engine/ stays pure.
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
//   * CONVERSE (living NPCs) — two (sometimes three) wandering civilians pair
//     up when they pass close: they walk to talk slots ~1.2 m apart, FACE each
//     other, and trade phase-offset nod-bobs + lean gestures for a dwell, then
//     part and wander on (a re-pair cooldown keeps it opportunistic).
//   * WORK (living NPCs) — agents assigned to authored WORK POINTS loop a task
//     read: CRATE-CARRY (walk to the crate, hoist, carry it leaning to B, drop,
//     walk back; the crate is a tinted box entity that rides in the arms),
//     CONSOLE-TEND (stand at a point facing the console, periodic lean-in bob),
//     SWEEP (slow line pacing between two points).
//   * PLAY (living NPCs) — a small knot in an open space doing a KICKABOUT: a
//     shared ball entity chip-passes agent-to-agent (kinematic lerp to the next
//     player's feet on a timer) while the players shuffle/reposition between
//     passes and face the ball. Where a ball doesn't fit (facility fringe), a
//     seated HAND-GAME pair: two agents crouched face-to-face trading
//     alternating gesture bobs.
//   * SCATTER + COWER on violence — CrowdSystem::onViolence(pos) sends every
//     agent within scatterRadius sprinting AWAY from the violence (clamped to
//     the room region), where it then COWERS (crouched, trembling-still).
//     Conversations dissolve, carriers DROP the crate where they stand, the
//     ball freezes where it lies.
//   * RETURN after calm — once calmTime passes with no further violence, the
//     cowering crowd stands back up and resumes its LIFE: civilians wander back
//     to their points, workers walk back to the (dropped) crate / console /
//     sweep line, players drift back to the knot and the ball starts moving
//     again.
//
// Counts are small (a club floor is 10-20 NPCs, a district edge zone 8-14), so
// every agent updates every frame with cheap dt-scaled math — no round-robin
// needed; still zero per-frame heap allocation. The host feeds onViolence()
// from its gunshot/melee/death events.
//
// REGION SAFETY (streamed city crowds): a CrowdSystem deployed inside a
// WorldStreamer region realize is LEDGER-OWNED — every entity/mesh it creates
// is captured into the region's ownership ledger and destroyed by the region
// teardown, NOT by this system. abandon() is the teardown-side contract: it
// forgets all entity ids/mesh handles WITHOUT touching the Scene/device (the
// ledger owns them) and returns the system to the unbuilt state so the next
// region realize can build() it fresh.

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
    Converse,    // paired chat: walk to the talk slot, face partner, nod/gesture
    Work,        // task loop at an authored work point (carry / console / sweep)
    Play,        // knot game: kickabout ball / seated hand-game pair
};
const char* crowdStateName(CrowdState s);

// What an agent IS in this deployment (assigned at build, drives calm-return).
enum class CrowdRole : uint32_t {
    Civilian = 0,   // idles / wanders / converses at the hangout points
    Worker,         // bound to one CrowdWorkPoint, loops its task read
    Gamer,          // bound to one CrowdPlaySpot knot
};

// One authored work point. `a` is the primary point (crate pickup / console
// stand / sweep end A); `b` is the secondary (crate drop-off / console FACING
// target / sweep end B).
struct CrowdWorkPoint {
    enum class Kind : uint32_t { Carry = 0, Console, Sweep };
    Kind  kind = Kind::Carry;
    float ax = 0.0f, az = 0.0f;
    float bx = 0.0f, bz = 0.0f;
};

// One authored play knot. ball=true => kickabout with a shared ball entity;
// ball=false => a seated hand-game PAIR (players is clamped to 2 then).
struct CrowdPlaySpot {
    float    cx = 0.0f, cz = 0.0f;
    uint32_t players = 4;
    bool     ball = true;
};

// Crowd configuration (authored by the host area: club / facility room / city
// district site).
struct CrowdConfig {
    uint32_t count   = 12;          // how many NPCs
    float    centerX = 0.0f;        // room/region center (world)
    float    centerZ = 0.0f;
    float    groundY = 0.0f;        // floor height the crowd stands on
    float    radius  = 10.0f;       // hard region clamp (agents never leave it)
    // Rect clamp for hallways/street bands: when BOTH are > 0 the region is the
    // axis-aligned rect center +- (halfX, halfZ) instead of the radius circle.
    float    halfX   = 0.0f;
    float    halfZ   = 0.0f;
    // PVS: roomId stamped on every crowd entity (agents + crates + balls).
    // kNoRoom (default) = always drawn (legacy club/level1 behavior). Canon
    // rooms pass their room index; streamed city sites pass
    // kStreamedExteriorRoom (the realize re-stamp writes the same value).
    uint32_t roomId  = kNoRoom;
    // Hangout points (world XZ pairs). Empty => a generated ring of 4 points
    // around the center at half the radius.
    std::vector<float> points;
    // LIVING NPCs: one Worker is assigned per work point, `players` Gamers per
    // play spot (both clamped to the agent count); the rest are Civilians.
    std::vector<CrowdWorkPoint> work;
    std::vector<CrowdPlaySpot>  play;
    bool     converse    = false;   // civilians opportunistically pair up to chat
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
    CrowdRole  role   = CrowdRole::Civilian;
    x3::phys::Vec3 pos{};
    x3::phys::Vec3 target{};
    float      yaw    = 0.0f;
    float      timer  = 0.0f;       // dwell / scatter-burst / talk countdown
    float      phase  = 0.0f;       // per-agent dance/tremble/gesture phase offset
    // CONVERSE: partner agent INDEX (kNoLink = not chatting) + re-pair cooldown.
    uint32_t   partner = kNoLink;
    float      chatCooldown = 0.0f;
    // WORK: index into cfg.work + the task micro-phase (see crowd.cpp).
    uint32_t   workIdx = kNoLink;
    uint32_t   workPhase = 0;
    // PLAY: index into cfg.play + this agent's slot in the knot ring.
    uint32_t   playIdx = kNoLink;
    uint32_t   slot = 0;
    // ---- Visual gesture OUTPUTS (skinned-citizen layer). update() computes a
    // per-frame bob / crouch / lean for the blockout transform; these mirror the
    // exact values it passed to writeTransform so a skinned visual layer
    // (app/crowd_skin.h) can apply the SAME gestures on top of a rigged
    // character without duplicating the behaviour math. Read-only for hosts.
    float      visBob = 0.0f;      // vertical bob offset (m)
    float      visCrouch = 1.0f;   // vertical scale (1 = standing, <1 = huddled/seated)
    float      visLean = 0.0f;     // torso pitch toward facing (radians)
};

// A shared prop entity the crowd animates kinematically (crate / ball).
struct CrowdProp {
    uint32_t entity = kNoLink;
    x3::phys::Vec3 pos{};
    bool     carried = false;       // crates: riding in a worker's arms
    uint32_t holder  = kNoLink;     // ball: agent index it sits with / flies to
    bool     inFlight = false;      // ball: mid-pass
    float    flightT = 0.0f, flightDur = 1.0f;
    x3::phys::Vec3 from{};
};

// The crowd layer. Build once into a Scene; update every frame; feed violence.
class CrowdSystem {
public:
    // Build `cfg.count` NPCs sharing one procedural humanoid mesh, seeded in a
    // loose cluster around the hangout points; workers seed at their work
    // points, gamers at their knot. Creates the crate/ball prop entities. Call
    // once (or again after abandon()).
    void build(const CrowdConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device);

    // Advance one frame (dt-scaled movement; dance sway; conversations; work
    // loops; ball passes; calm countdown).
    void update(float dt, Scene& scene);

    // Violence at `pos` (gunshot / melee / a death): every agent within
    // scatterRadius sprints away then cowers; carriers drop the crate, the ball
    // freezes; resets the calm countdown. A shot with NO agent in earshot is
    // ignored (a distant indoor gunfight never pauses a street kickabout).
    void onViolence(const x3::phys::Vec3& pos);

    // REGION SAFETY: forget every entity id / mesh handle WITHOUT destroying
    // anything (the region ownership ledger owns and tears them down) and go
    // back to the unbuilt state. Call from the region teardown hook BEFORE any
    // slot is released; update()/onViolence() are no-ops afterwards.
    void abandon();

    // ---- Queries (host HUD / self-test) ----
    bool     built() const { return m_built; }
    const CrowdConfig& config() const { return m_cfg; }
    uint32_t agentCount() const { return (uint32_t)m_agents.size(); }
    const CrowdAgent& agent(uint32_t i) const { return m_agents[i]; }
    uint32_t countInState(CrowdState s) const;
    // True when no violence is pending (the calm countdown has expired).
    bool     calm() const { return m_calmTimer <= 0.0f; }
    // Props (self-test proof): one crate per Carry work point, one ball per
    // ball play spot (index == play-spot index; entity kNoLink for hand-game).
    uint32_t crateCount() const { return (uint32_t)m_crates.size(); }
    const CrowdProp& crate(uint32_t i) const { return m_crates[i]; }
    uint32_t ballCount() const { return (uint32_t)m_balls.size(); }
    const CrowdProp& ball(uint32_t i) const { return m_balls[i]; }

private:
    void writeTransform(CrowdAgent& a, Scene& scene, float bob, float crouch, float lean);
    void writePropTransform(uint32_t entity, Scene& scene,
                            float x, float y, float z, float yaw, float s);
    void clampToRegion(float& x, float& z) const;
    void faceToward(CrowdAgent& a, float x, float z, float dt, float rate);
    void endConverse(CrowdAgent& a);
    void resumeAfterCalm(CrowdAgent& a);
    void updateConversePairing();
    void updateBalls(float dt, Scene& scene);
    uint32_t rng();
    float frand();   // 0..1

    bool        m_built = false;
    CrowdConfig m_cfg{};
    std::vector<CrowdAgent> m_agents;
    std::vector<float> m_points;     // resolved hangout points (x,z pairs)
    std::vector<CrowdProp> m_crates; // parallel to the Carry work points
    std::vector<uint32_t>  m_crateForWork; // work index -> crate index (or kNoLink)
    std::vector<CrowdProp> m_balls;  // parallel to cfg.play (entity kNoLink = hand-game)
    x3::rhi::MeshHandle m_mesh{};
    x3::rhi::MeshHandle m_crateMesh{};
    x3::rhi::MeshHandle m_ballMesh{};
    float       m_calmTimer = 0.0f;  // > 0 while violence is fresh
    float       m_time = 0.0f;       // dance/gesture clock
    uint32_t    m_rngState = 0x9E3779B9u;
};

// Headless self-test (--test-crowd). Asserts the ambient core: (C1) build
// counts + one shared mesh; (C2) the idling/wandering crowd stays inside its
// region; (C3) violence scatters nearby agents AWAY from the point; (C4)
// scattered agents settle into Cower while violence is fresh; (C5) after
// calmTime of quiet the crowd returns to Idle/Wander; (C6) long ticking leaks
// no meshes/entities. Then the LIVING-NPC layer: (C7) wandering civilians pair
// into conversations, stand ~1.2 m apart and FACE each other; (C8) the work
// loops reach their points (crate rides to B and lands there; console tended
// in place; sweep paces both ends); (C9) the kickabout ball passes between
// distinct players and stays in the knot; (C10) violence scatters EVERY state
// (chat dissolved, crate dropped, ball frozen); (C11) after calm the workers
// work, the players play, the civilians wander — all in-region; (C12) the
// extended system leaks nothing across long ticking; (C13) abandon() forgets
// without touching the scene and a rebuild works. Prints "crowd: X/Y passed";
// returns true iff all pass. No window/Vulkan.
bool runCrowdSelfTest();

} // namespace x3::game
