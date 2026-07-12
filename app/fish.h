#pragma once
// FISH — the river/sea LIVES (living-world, W10 water pass). Game/slice code
// only; engine/ stays pure.
//
// Ambient SCHOOLS of fish in the world water (THE RIVER's reach past the
// facility + the sea shallows at the estuary). Same SIMULACRA stance as the
// ambient ecology (app/ecology.h) and the crowds (app/crowd.h): no physics
// bodies, no skinning, no per-agent model loads — but the fish must LOOK like
// fish, which the first (box-built) pass did not.
//
// THE MESH (art pass 2 — "how hard is it to make a fish? LOL"):
//   * A LOFTED body: ellipse rings swept along the spine, the ring radius
//     following a smooth profile (pointed snout -> deepest at ~30% back ->
//     pinched tail root). Laterally compressed (width ~0.45 of height), like a
//     real fish. A forked CAUDAL fin, a dorsal blade and a pectoral pair.
//   * COUNTERSHADING baked into the UVs: v = up-ness of the ring vertex, sampled
//     from ONE tiny gradient texture (dark olive-steel back -> pale silver belly).
//     That is what makes it read as a fish instead of a white brick — and it is
//     what makes a BELLY-UP corpse read: rolled 180 deg, the pale side faces the
//     sky. Per-school tint + per-fish jitter multiply the gradient.
//   * THE SWIM: the body is THREE hinged pieces (head / mid / tail+caudal) that
//     share the fish transform. A travelling sine runs down the spine — the mid
//     sweeps a little, the tail sweeps more, one phase behind it — so the body
//     S-FLEXES and the tail beats. The fish also BANKS into its turns (roll from
//     the measured yaw rate). A fish that translates without flexing reads as a
//     floating prop.
//   * 3 shared meshes + 1 shared texture for the whole shoal; 3 Scene entities
//     per fish, transforms rewritten each frame. Zero per-frame allocation.
//
// THE SIM:
//   * SCHOOLING: each school has a drifting CENTER that probes the water query
//     ahead and turns when it would beach itself (a river school follows the
//     channel; a sea school mills in the shallows). Fish steer to their slot
//     (cohesion), push apart when crowded (separation) and share the school
//     heading (alignment) — boids-lite, O(n^2) inside an 8-13 fish school.
//   * DEPTH-BOUND between the bed (+ depthMin) and just under the surface.
//   * FLEE: the player inside fleeRadius scatters them; they re-form after.
//   * DEAD FISH (the water zap, app/waterzap.h): killWithin() rolls every fish in
//     the radius BELLY-UP; the corpse floats up INTO the surface plane (pale side
//     to the sky), drifts with the current, lolls, and despawns after deadLinger.
//   * DETERMINISTIC (one LCG); range-gated per school; the host stamps
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
    // Tint MULTIPLIED over the countershading gradient (so a school reads olive /
    // steel / bronze while every fish keeps its dark back + pale belly).
    float    tint[3] = { 1.0f, 1.0f, 1.0f };
};

// Whole-shoal config + behavior tunables.
struct FishConfig {
    std::vector<FishSchoolDesc> schools;
    uint32_t roomId       = kNoRoom;   // PVS tag stamped on every fish entity
    float activeRadius    = 140.0f;    // school simulated/drawn within this of the player (m)
    float fleeRadius      = 2.5f;      // player closer than this to a fish -> it bolts
    float fleeSpeed       = 4.0f;      // flee burst speed (m/s)
    float fleeTime        = 1.8f;      // seconds a flee burst lasts
    float separation      = 0.85f;     // closer than this to a schoolmate -> push apart (m)
    float turnRate        = 3.2f;      // yaw slew (rad/s)
    float depthMin        = 0.45f;     // never closer than this to the bed (m)
    float depthBelowSurf  = 0.55f;     // never closer than this to the surface (m)
    // THE SWIM: a travelling sine down the spine. The mid piece sweeps midAmp,
    // the tail piece tailAmp one `lag` behind it — that phase offset IS the
    // S-flex (equal phases would just wag the whole fish rigidly).
    float beatHz          = 2.6f;      // tail beats per second (x1.9 while fleeing)
    float midAmp          = 0.16f;     // mid-body sweep (rad)
    float tailAmp         = 0.42f;     // tail sweep (rad)
    float beatLag         = 1.05f;     // tail phase lag behind the mid (rad)
    float bankPerTurn     = 0.30f;     // roll per rad/s of yaw rate (banking)
    float bankMax         = 0.55f;     // roll clamp (rad)
    float size            = 0.72f;     // base visual scale (~0.6-0.9 m river fish)
    float deadRise        = 0.55f;     // belly-up rise to the surface (m/s)
    float deadDrift       = 0.40f;     // corpse drift with the current (m/s)
    float deadLinger      = 26.0f;     // seconds a corpse floats before it despawns
    uint32_t seed         = 0xF15Fu;   // LCG seed (determinism)
};

// One fish. Three entities (head / mid / tail) share one transform chain.
struct Fish {
    uint32_t entHead = kNoLink;
    uint32_t entMid  = kNoLink;
    uint32_t entTail = kNoLink;
    uint32_t school  = 0;
    float x = 0, y = 0, z = 0;
    float yaw   = 0.0f;
    float roll  = 0.0f;      // bank into turns; PI when DEAD (belly-up)
    float midW  = 0.0f;      // mid-piece hinge angle (rad) — the S-flex
    float tailW = 0.0f;      // tail-piece hinge angle (rad)
    float size  = 1.0f;      // per-fish size multiplier (on cfg.size)
    float phase = 0.0f;      // beat phase offset
    float speed = 1.0f;      // per-fish cruise speed
    float fleeT = 0.0f;      // > 0 while bolting from the player
    // Slot in the school (local, rotated by the school heading): the boids
    // cohesion target. slotD = the fish's preferred depth offset below the top
    // of its depth band, so a school reads as a 3D shoal not a flat sheet.
    float slotX = 0.0f, slotZ = 0.0f, slotD = 0.0f;
    bool  dead  = false;     // ZAPPED: belly-up, floating, drifting
    float deadT = 0.0f;      // seconds since death (despawns at deadLinger)
    bool  gone  = false;     // corpse despawned (entities hidden for good)
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

    // Build every school (3 shared lofted meshes + 1 shared countershading
    // gradient; 3 entities per fish). Fish are seeded on their slots around the
    // school center at real depths. Schools whose center is DRY are skipped
    // (logged) — a school never spawns on land.
    void build(const FishConfig& cfg, Scene& scene, x3::rhi::IRenderDevice& device);

    // Advance one frame: school drift (water-probed), boids steering, flee bursts,
    // depth clamp, the travelling-sine swim + bank, corpse float/loll/despawn.
    void update(float dt, Scene& scene, const x3::phys::Vec3& playerPos);

    // THE ZAP (app/waterzap.h): kill every LIVE fish whose XZ distance to
    // (cx,cz) is <= radius — they roll belly-up and float. Returns the number
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
    void  setVisible(Fish& f, Scene& scene, bool vis);
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
    x3::rhi::MeshHandle     m_meshHead{}, m_meshMid{}, m_meshTail{};
    x3::rhi::TextureHandle  m_skin{};     // the countershading gradient
    float       m_time = 0.0f;
    uint32_t    m_rngState = 0xF15Fu;
};

} // namespace x3::game
