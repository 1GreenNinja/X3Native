#pragma once
// NPC LIFE — the LIVING CITY layer (W5 / task #41). Game/slice code only; engine/ stays pure.
//
// The soul layer on top of the neon district's ambient crowd: authored OCCUPATION NPCs
// (the 12 Fable roster archetypes, D:\GameDev\NPC_LIFE_ROSTER.md) that have actual LIVES —
//   * a DAILY LOOP: each NPC carries a home/work/leisure schedule and a time-of-day state
//     machine (AtHome -> ToWork -> AtWork -> ToLeisure -> AtLeisure -> ToHome) that walks
//     the street grid to REAL destinations, not a random dwell wander. Deterministic seed.
//   * a SCAN-CARD: name (procedural) + occupation + ONE telling human detail (the Watch-
//     Dogs hook) + a hack payload + karma rule, surfaced through the EXISTING NPC-scan
//     hackable (HackableType::Npc) -> the HoloPanel card. Karma WATCHES THE VULNERABLE:
//     hacking the kid / off-shift drone / baker costs karma; the fixer / robber are neutral.
//   * a VOICE: a small-talk register per archetype (the baker and the fixer must never
//     sound the same). Optionally LLM-generated per-instance (VIGIL/llama), with the
//     hand-authored fallback line when the model is off (cached per NPC; never stalls).
//   * THE ROBBERY SET-PIECE: the Bank Robber cases the bank, strikes on a timer, trips the
//     alarm (AlertSystem heat via the alarm sink), the Street Cops converge, and the robber
//     flees toward the freeway. The player can HINDER by hacking a converging cop (spoof his
//     radio -> he loses the scent) or the robber (pre-warn). One emergent scene.
//   * FREEWAY TRAFFIC: couriers on bikes (+ a few cars) drive the drivable freeway ribbon
//     (worldFreewaySampleArc) into/out of the city and despawn/recycle at the bounds.
//
// PURITY / STANCE: same SIMULACRA layer as crowd.* / ecology.* — agents are pure kinematic
// Scene entities sharing a few procedural meshes (no physics bodies, no skinning, no per-
// agent loads). The system OWNS no AlertSystem/TimelineState/render state: the host wires
// the alarm sink to the REAL AlertSystem and the scan hackables carry their own karma so
// the whole machine is headlessly testable (--test-npclife) and reusable by any district.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"   // x3::phys::Vec3

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace x3::llm { class ILlmSystem; }     // optional LLM flavor (VIGIL/llama)

namespace x3::game {

class HackableRegistry;                      // hackables.h — the scan-card layer

// The 12 roster occupations (order == NPC_LIFE_ROSTER.md). Each = schedule + persona +
// scan-card + voice. Cycle + vary names/details procedurally per instance.
enum class Archetype : uint32_t {
    HotDogVendor = 0,   // static cart on the main drag
    BankRobber,         // the set-piece seed
    Electrician,        // junction-box work sites
    Programmer,         // hunched in the noodle bar
    Baker,              // pre-dawn open, gone by afternoon
    Gardener,           // tends the median planters
    Courier,            // rides the freeway (motorcyclist)
    StreetCop,          // patrol + responds to the alarm
    Preacher,           // static street-corner prophet
    OffShiftDrone,      // the tired ration-line mass
    Fixer,              // alley black-market dealer
    Kid,                // darts between adults (DON'T hack)
    Count
};
constexpr uint32_t kArchetypeCount = (uint32_t)Archetype::Count;

// Occupation label for the scan-card ROLE line ("HOT-DOG VENDOR" / "STREET COP" / ...).
const char* archetypeName(Archetype a);

// The time-of-day activity a scheduled NPC is in (derived from the day clock + schedule).
enum class NpcActivity : uint32_t {
    AtHome = 0, ToWork, AtWork, ToLeisure, AtLeisure, ToHome, Count
};
const char* npcActivityName(NpcActivity s);

// The robbery set-piece phases.
enum class RobberyPhase : uint32_t {
    Idle = 0,   // no robber yet / disabled
    Casing,     // at the cafe by the bank, watching the guard rotation
    Strike,     // moving in / on the bank
    Alarm,      // alarm tripped, cops alerted
    Flee,       // running for the freeway
    Escaped,    // reached the freeway exit
    Caught,     // a cop closed the distance
    Count
};
const char* robberyPhaseName(RobberyPhase p);

// The authored SOUL for one archetype (the roster's voice + details + hack rules). Static.
struct Persona {
    Archetype   arch;
    const char* occupation;                 // scan-card ROLE
    const char* detail[3];                  // telling details (pick one per instance)
    uint32_t    detailCount;
    const char* voice[3];                   // small-talk lines (the register IS the identity)
    uint32_t    voiceCount;
    int         skimCredits;                // scan-card skim amount
    int         hackKarma;                  // karma delta when hacked (<0 == vulnerable)
    float       tint[3];                    // body color (linear RGB)
    float       scale;                      // visual scale
    bool        isStatic;                   // holds a fixed post (vendor/preacher/fixer/programmer)
    bool        ridesFreeway;               // courier — spawns as freeway traffic too
    bool        patrols;                    // street cop — converges on the robbery
};
// The persona table (indexed by archetype). Read-only.
const Persona& persona(Archetype a);

// A resolved daily schedule: three world XZ posts + the day-fraction windows for each.
struct NpcSchedule {
    float homeX = 0, homeZ = 0;
    float workX = 0, workZ = 0;
    float leisureX = 0, leisureZ = 0;
    float workStart = 0.30f, workEnd = 0.62f;      // day-fraction [0,1)
    float leisureStart = 0.62f, leisureEnd = 0.85f;
};

// One living NPC.
struct NpcAgent {
    Archetype   arch = Archetype::OffShiftDrone;
    uint32_t    entity     = kNoLink;       // body scene entity
    uint32_t    propEntity = kNoLink;       // cart / bike (optional)
    uint32_t    hackReg    = kNoLink;        // index into the HackableRegistry (scan target)
    NpcActivity activity   = NpcActivity::AtHome;
    NpcSchedule sched{};
    x3::phys::Vec3 pos{}, target{}, via{};
    bool        viaActive = false;          // routing via the drag centerline (street-follow)
    float       yaw   = 0.0f;
    float       speed = 1.1f;
    uint32_t    seed  = 0;
    std::string name;                       // procedural scan-card name
    int         detailIdx = 0;              // which telling detail this instance carries
    // ---- optional LLM flavor (VIGIL/llama; dormant when the model is off) ----
    uint32_t    llmChat   = 0;              // x3::llm::ChatId (0 == none)
    bool        llmPending = false;         // a per-instance detail is still generating
    // ---- robbery / cop state ----
    bool        converge  = false;          // cop: converging on the bank
    bool        spoofed   = false;          // cop: radio spoofed (player hacked -> loses scent)
    bool        planKnown = false;          // robber: player pre-warned
    // ---- freeway mover ----
    bool        onFreeway = false;
    float       arc      = 0.0f;            // parameter along worldFreewaySampleArc [0,1]
    float       arcSpeed = 0.05f;           // per-second
    float       arcLane  = 0.0f;            // lane offset (± across the deck)
    bool        arcForward = true;
};

// Config for the living layer.
struct NpcLifeConfig {
    float    centerX = -600.0f, centerZ = 500.0f;   // district center (the main drag)
    float    groundY = 0.0f;                         // street level (feet)
    float    dayLengthSeconds = 240.0f;              // one full home->work->leisure->home loop
    float    startFraction    = 0.34f;               // begin mid-morning (workers heading in)
    uint32_t seed             = 0x5EED1CE7u;
    float    robberyAtFraction = 0.58f;              // heist strikes at dusk (<0 disables auto)
    int      freewayMovers     = 6;                  // couriers + cars riding the freeway
    bool     registerScans     = true;               // register each NPC as an NPC-scan hackable
    // Occlusion-cull room stamped on EVERY entity this system adds (bodies, props,
    // scan markers, freeway movers). Default kNoRoom == always visible (headless /
    // self-test). A streamed host passes its district room (e.g. kStreamedExteriorRoom)
    // so the living NPCs are PVS-culled with the city exactly like the crowd blockouts.
    uint32_t roomId            = kNoRoom;
    // LEISURE MAGNET (Lane 4 — "citizens ENTER buildings"): when set, the
    // social archetypes (Electrician / Courier / Gardener / OffShiftDrone)
    // take their leisure AT this spot (the noodle bar counter) instead of
    // their default corners — patrons appear on schedule and leave on it.
    bool     leisureMagnet     = false;
    float    leisureMagnetX    = 0.0f, leisureMagnetZ = 0.0f;
};

// Alarm sink: the host wires this to AlertSystem::reportGunshot so the heist trips real heat.
using NpcAlarmFn = std::function<void(const x3::phys::Vec3& pos, int heat)>;

// The living-city system. Build once into a Scene; update every frame.
class NpcLife {
public:
    // Build the authored archetype mix + freeway traffic. When `hax` is passed, each walking
    // NPC is registered as an NPC-scan hackable (scan-card name/occupation/telling-detail +
    // per-archetype karma). `llm` (optional, may be null / model-off) generates the telling
    // detail per instance; the hand-authored roster line is the fallback. Call once.
    void build(const NpcLifeConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device,
               HackableRegistry* hax = nullptr, x3::llm::ILlmSystem* llm = nullptr);

    // Advance one frame: the day clock, every schedule state machine + street routing, the
    // robbery FSM + cop convergence, and freeway traffic. Drains any pending LLM details.
    void update(float dt, Scene& scene);

    // The host wires this to the REAL AlertSystem (reportGunshot at the bank).
    void setAlarmSink(const NpcAlarmFn& fn) { m_alarm = fn; }

    // The player hacked the NPC registered at HackableRegistry index `hackRegIndex`:
    // spoof a converging cop (misdirect the heat) or pre-warn about the robber. No-op if the
    // index maps to no NpcLife agent.
    void notifyHacked(uint32_t hackRegIndex);

    // Force the robbery to strike now (a trigger volume / the auto-timer / the self-test).
    void triggerRobbery();

    // ---- PLAY-AS: hand one agent's body to the player ----
    // While an agent is "controlled" its daily schedule is paused in update() and the
    // host drives its body directly via driveControlled(). Pass -1 to release (the
    // agent resumes its schedule from wherever it was left).
    void setControlled(int idx) { m_controlled = idx; }
    int  controlled() const { return m_controlled; }
    // Place the controlled agent's body (world feet position + facing). No-op if none.
    void driveControlled(float x, float y, float z, float yaw);

    // ---- The day clock ----
    void  setDayFraction(float t);
    float dayFraction() const { return m_t; }

    // ---- Queries (host HUD / self-test) ----
    bool     built() const { return m_built; }
    uint32_t agentCount() const { return (uint32_t)m_agents.size(); }
    const NpcAgent& agent(uint32_t i) const { return m_agents[i]; }
    uint32_t countArchetype(Archetype a) const;
    uint32_t countActivity(NpcActivity s) const;
    RobberyPhase robberyPhase() const { return m_robPhase; }
    int      robberIndex() const { return m_robber; }
    uint32_t convergingCops() const;
    uint32_t freewayCount() const;
    // Copy up to `max` live street-cop positions into `out` (host feeds them to AlertSystem
    // as observers so the heist gunshot is HEARD). Returns the count written.
    uint32_t copPositions(x3::phys::Vec3* out, uint32_t max) const;
    // The bank position (alarm origin) — for the host's observer/heat wiring.
    x3::phys::Vec3 bankPos() const { return m_bankPos; }

private:
    void   writeTransform(NpcAgent& a, Scene& scene) const;
    void   retarget(NpcAgent& a);                     // pick the next destination for the activity
    NpcActivity desiredActivity(const NpcAgent& a) const;
    void   updateWalker(NpcAgent& a, float dt);
    void   updateFreeway(NpcAgent& a, float dt);
    void   updateRobbery(float dt);
    uint32_t rng(uint32_t& s) const;
    std::string makeName(uint32_t& s) const;

    bool        m_built = false;
    NpcLifeConfig m_cfg{};
    std::vector<NpcAgent> m_agents;
    x3::rhi::MeshHandle m_bodyMesh{};       // shared humanoid
    x3::rhi::MeshHandle m_cartMesh{};       // vendor cart
    x3::rhi::MeshHandle m_bikeMesh{};       // courier motorcycle
    float       m_t = 0.0f;                 // normalized day clock [0,1)
    NpcAlarmFn  m_alarm;
    // Robbery.
    RobberyPhase m_robPhase = RobberyPhase::Idle;
    int          m_robber   = -1;           // index of the bank-robber agent
    float        m_robTimer = 0.0f;
    bool         m_alarmFired = false;
    x3::phys::Vec3 m_bankPos{};
    x3::phys::Vec3 m_freewayExit{};
    HackableRegistry* m_hax = nullptr;
    x3::llm::ILlmSystem* m_llm = nullptr;
    int          m_controlled = -1;         // PLAY-AS: agent the host is driving (-1 == none)
};

// Headless self-test (--test-npclife). Asserts: (N1) the archetype mix spawns with one
// shared body mesh; (N2) schedules drive the state machine — at work-hours the day-workers
// are AtWork/ToWork, at night they head home; (N3) street routing keeps walkers converging
// on real destinations; (N4) the robbery fires the alarm sink + flips nearby cops to
// converge + the robber flees toward the freeway; (N5) the karma rules — a REAL registry +
// TimelineState: hacking the kid/drone/baker costs karma, the fixer/robber are neutral;
// (N6) freeway movers ride the ribbon + recycle at the bounds; (N7) no mesh/entity leaks
// across long ticking. Prints "npclife: X/Y passed"; returns true iff all pass. No window.
bool runNpcLifeSelfTest();

} // namespace x3::game
