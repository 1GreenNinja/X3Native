#pragma once
// FISH — the river/sea LIVES (living-world, W10 water pass). Game/slice code
// only; engine/ stays pure.
//
// Ambient SCHOOLS of fish in the world water (THE RIVER's reach past the
// facility + the sea shallows at the estuary). Same SIMULACRA stance as the
// ambient ecology (app/ecology.h) and the crowds (app/crowd.h):
//
//   * ONE shared procedural fish mesh (tapered body + tail fin + dorsal),
//     created once at build(); each fish is a Scene entity whose transform is
//     rewritten as it swims. Per-fish TINT (silver / olive / copper) + a slight
//     size variance. No physics bodies, no skinning, no per-agent model loads.
//   * SCHOOLING: each school has a drifting CENTER that walks along the water
//     (it probes the water query ahead and turns when it would beach itself —
//     so a river school follows the channel and a sea school mills in the
//     shallows). Fish steer to their own slot in the school (cohesion), push
//     apart when they crowd (separation) and share the school heading
//     (alignment) — boids-lite, O(n^2) inside a 8-14 fish school, which is
//     nothing.
//   * DEPTH-BOUND: a fish lives between the bed (+ depthMin) and just under the
//     surface (- depthBelowSurface), so it reads as a shape UNDER the water from
//     the bank and swims past you when you are in it.
//   * FLEE: the player inside fleeRadius scatters the school (they burst away,
//     then re-form on their slots) — swimming through a school PARTS it.
//   * DEAD FISH (the water zap, app/waterzap.h): killWithin() flips every fish
//     in the radius BELLY-UP; the corpse floats to the surface, drifts with the
//     current and despawns after deadLinger seconds.
//   * DETERMINISTIC: one LCG seeded from cfg.seed at build; update() is pure
//     dt-scaled math with zero per-frame allocation.
//   * RANGE-GATED: a school beyond activeRadius of the player is neither
//     simulated nor drawn (entities hidden). The host additionally stamps
//     cfg.roomId = kStreamedExteriorRoom so the outdoor PVS gates the draw.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace x3::game {

// The water-surface feed: returns the water surface Y over world (x,z), or a
// very-negative sentinel (< kFishDryTest) when the point is DRY. The canon host
// passes x3::game::worldWaterLevelAt (app/terrain.h); the self-test injects a
// synthetic slab. Fish never include terrain.h.
using FishWaterFn = std::function<float(float x, float z)>;
// The BED feed: the ground/terrain height under (x,z) — the floor a fish will
// not swim through. Optional: unset => the bed is treated as (surface - 30).
using FishBedFn = std::function<float(float x, float z)>;
// Anything below this is the "no water here" sentinel (matches kWorldWaterDry).
constexpr float kFishDryTest = -1.0e30f;

// One authored school.
struct FishSchoolDesc {
    float    centerX = 0.0f;      // seed center (world XZ; must be over water)
    float    centerZ = 0.0f;
    uint32_t count   = 10;        // fish in this school
    float    spread  = 5.0f;      // school radius (m) — the slot ring
    float    heading = 0.0f;      // initial swim heading (radians, world XZ)
    float    speed   = 1.0f;      // cruise speed of the school center (m/s)
    float    tint[3] = { 0.72f, 0.76f, 0.80f };  // base body tint (silver default)
};

// Whole-shoal config + behavior tunables.
struct FishConfig {
    std::vector<FishSchoolDesc> schools;
    uint32_t roomId       = kNoRoom;   // PVS tag stamped on every fish entity
    float activeRadius    = 140.0f;    // school simulated/drawn within this of the player (m)
    float fleeRadius      = 2.5f;      // player closer than this to a fish -> it bolts
                                       // (a river fish lets you get within a couple of
                                       //  metres; 5 m made the whole shoal bolt at the
                                       //  sight of a man on the bank)
    float fleeSpeed       = 4.0f;      // flee burst speed (m/s)
    float fleeTime        = 1.8f;      // seconds a flee burst lasts
    float separation      = 0.85f;     // closer than this to a schoolmate -> push apart (m)
    float turnRate        = 3.2f;      // yaw slew (rad/s)
    float depthMin        = 0.45f;     // never closer than this to the bed (m)
    float depthBelowSurf  = 0.55f;     // never closer than this to the surface (m)
    float wiggleHz        = 3.2f;      // tail-wiggle frequency (yaw sway)
    float wiggleAmp       = 0.20f;     // tail-wiggle amplitude (rad)
    float size            = 0.60f;     // base visual scale (~0.55-0.75 m river fish)
    float deadRise        = 0.55f;     // belly-up rise to the surface (m/s)
    float deadDrift       = 0.40f;     // corpse drift with the current (m/s)
    float deadLinger      = 26.0f;     // seconds a corpse floats before it despawns
    uint32_t seed         = 0xF15Fu;   // LCG seed (determinism)
};

// One fish.
struct Fish {
    uint32_t entity = kNoLink;
    uint32_t school = 0;
    float x = 0, y = 0, z = 0;
    float yaw   = 0.0f;
    float size  = 1.0f;      // per-fish size multiplier (on cfg.size)
    float phase = 0.0f;      // wiggle phase offset
    float speed = 1.0f;      // per-fish cruise speed
    float fleeT = 0.0f;      // > 0 while bolting from the player
    // Slot in the school (local, rotated by the school heading): the boids
    // cohesion target. slotD = the fish's preferred depth offset below the top
    // of its depth band, so a school reads as a 3D shoal not a flat sheet.
    float slotX = 0.0f, slotZ = 0.0f, slotD = 0.0f;
    bool  dead  = false;     // ZAPPED: belly-up, rising, drifting
    float deadT = 0.0f;      // seconds since death (despawns at deadLinger)
    bool  gone  = false;     // corpse despawned (entity hidden for good)
};

// One school (the drifting center the fish orbit).
struct FishSchool {
    float cx = 0, cz = 0;    // center (world XZ)
    float heading = 0.0f;    // swim direction (radians)
    float speed = 1.0f;      // center drift speed (m/s)
    bool  active = false;    // player within activeRadius
};

class FishSystem {
public:
    // Wire the water/bed feeds BEFORE build() (build seeds fish at real depths).
    void setWaterQuery(FishWaterFn fn) { m_water = std::move(fn); }
    void setBedQuery(FishBedFn fn)     { m_bed   = std::move(fn); }

    // Build every school (one shared mesh + one Entity per fish). Fish are seeded
    // on their slots around the school center at real depths. Schools whose center
    // is DRY are skipped (logged) — a school never spawns on land.
    void build(const FishConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device);

    // Advance one frame: school drift (water-probed), boids steering, flee bursts,
    // depth clamp, tail wiggle, corpse float/despawn. dt-scaled; no allocation.
    void update(float dt, Scene& scene, const x3::phys::Vec3& playerPos);

    // THE ZAP (app/waterzap.h): kill every LIVE fish whose XZ distance to
    // (cx,cz) is <= radius — they flip belly-up and float. Returns the number
    // killed. Fish outside the radius are untouched.
    uint32_t killWithin(float cx, float cz, float radius);

    // ---- Queries (HUD / self-test) ----
    bool built() const { return m_built; }
    const FishConfig& config() const { return m_cfg; }
    uint32_t fishCount() const { return (uint32_t)m_fish.size(); }
    const Fish& fish(uint32_t i) const { return m_fish[i]; }
    uint32_t schoolCount() const { return (uint32_t)m_schools.size(); }
    const FishSchool& school(uint32_t i) const { return m_schools[i]; }
    uint32_t aliveCount() const;
    uint32_t deadCount() const;    // dead but not yet despawned
    uint32_t activeCount() const;  // fish in an active (in-range) school

private:
    void  writeTransform(Fish& f, Scene& scene);
    float waterAt(float x, float z) const;
    float bedAt(float x, float z, float surface) const;
    uint32_t rng();
    float frand();   // 0..1

    bool        m_built = false;
    FishConfig  m_cfg{};
    FishWaterFn m_water;
    FishBedFn   m_bed;
    std::vector<Fish>       m_fish;
    std::vector<FishSchool> m_schools;
    x3::rhi::MeshHandle     m_mesh{};
    float       m_time = 0.0f;
    uint32_t    m_rngState = 0xF15Fu;
};

} // namespace x3::game
