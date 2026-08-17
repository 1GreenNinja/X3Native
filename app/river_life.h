#pragma once
// ---------------------------------------------------------------------------
// RIVER LIFE — "Put fish, speedboats in please.. i want to see the water!"
//
// The river at Bridge No.1 (app/river_bridge.h) holds water; this module makes
// that water LIVED-IN, and it builds NOTHING that already exists:
//
//   * FISH      — the existing FishSystem (app/fish.h: pose-baked Rodin species
//                 with the procedural-loft fallback), scoped to the bridge
//                 reach: schools seeded on the river's own spline nodes
//                 (worldRiverNodes) nearest the crossing, water/bed fed by the
//                 same worldWaterLevelAt / terrainHeightAtWorld queries the
//                 canon world uses. Water only — a school never spawns dry.
//   * SPEEDBOATS — two BoatDemo hulls (app/vehicle.h: the proven Jolt buoyancy
//                 controller), each running a back-and-forth patrol lane along
//                 the reach on its own side of the bridge. Autopilot is a
//                 heading PD on the hull's real attitude: steer toward the
//                 waypoint, flip ends on arrival. Sea level = the bridge
//                 plan's waterY — the SAME height the rendered Gerstner plane
//                 uses, so the hulls sit ON the visible water by construction.
//   * DRIVERS   — one skinned character standing at each helm: the crowd-skin
//                 inert-prop pattern (MonsterSystem, chaseSpeed 0 / damage 0 /
//                 noBody, pose fed per frame from the hull transform — the
//                 Club 1127 "character rides a car" precedent, verbatim).
//   * WAKES     — stern foam + spray through IRenderDevice::submitParticles
//                 (the engine's own billboard pass; the same pipe the rain and
//                 the wheel smoke ride). CPU pool, zero per-frame allocation.
//   * SOUND     — one outboard loop per boat (assets/audio/vehicles/
//                 outboard_loop.wav, synthesized by tools/gen_outboard_audio.py
//                 on gen_engine_bank.py's machinery), startLoop3D + per-frame
//                 setLoopPosition so the boats genuinely doppler/pan past, a
//                 small per-boat detune so the pair beat like real engines.
//
// Bounded-water law: everything here keys off the river spline + the bridge
// plan; nothing floods, nothing spawns outside the channel.
// ---------------------------------------------------------------------------
#include "fish.h"
#include "vehicle.h"
#include "river_bridge.h"

#include "engine/audio/IAudioSystem.h"

#include <memory>
#include <vector>

namespace x3::game {

class MonsterSystem;

class RiverLife {
public:
    // Out-of-line (river_life.cpp): Boat holds unique_ptr<MonsterSystem>, and
    // MonsterSystem is only forward-declared here — the implicit destructor
    // must instantiate where the type is complete, not in every host TU.
    RiverLife();
    ~RiverLife();

    // Build fish schools + two boats + their drivers around the bridge
    // crossing. `audio` may be null (headless capture path stays silent).
    // Returns false (and builds nothing) when the plan is not ok.
    bool build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& phys, x3::audio::IAudioSystem* audio,
               const RiverBridgePlan& plan);

    // BEFORE the host's phys->step(): autopilot input + buoyancy preStep.
    void prePhysics(float dt);

    // AFTER phys->step() AND scene.update(): boat postStep, driver pose-follow,
    // fish sim, wake FX sim, loop emitter positions. `focus` gates the fish
    // schools' active radius (pass the camera/player position).
    void postPhysics(float dt, Scene& scene, x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& phys, x3::audio::IAudioSystem* audio,
                     const x3::phys::Vec3& focus);

    // Between beginFrame/endFrame: boat hulls + drivers + wake particles.
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                const Scene& scene);

    void shutdown(x3::audio::IAudioSystem* audio);

    // ---- Queries (boot log / self-checks) ----
    bool     built() const { return m_built; }
    uint32_t fishCount() const { return m_fish.fishCount(); }
    uint32_t schoolCount() const { return m_fish.schoolCount(); }
    uint32_t boatCount() const { return (uint32_t)m_boats.size(); }
    // Hull world position of boat `i` (bounds/eye-gate checks).
    void     boatPos(uint32_t i, float out[3]) const;
    float    boatSpeed(uint32_t i) const;

private:
    struct Boat {
        BoatDemo demo;
        bool     ok = false;
        // Patrol lane: two ends on the river centreline, one side of the bridge.
        float ax = 0, az = 0, bx = 0, bz = 0;
        int   target = 1;              // 0 -> (ax,az), 1 -> (bx,bz)
        // Hull heading (radians, atan2(fwd.z, fwd.x)) read off the REAL body
        // attitude each postPhysics; prev value feeds the PD's yaw-rate term.
        float heading = 0.0f, headingPrev = 0.0f;
        bool  haveHeading = false;
        std::unique_ptr<MonsterSystem> driver;
        x3::audio::LoopHandle loop{};
        float pitch = 1.0f;            // per-boat detune on the shared loop
        float wakeAcc = 0.0f;          // spawn accumulator
    };

    struct Puff {                      // one wake foam/spray billboard
        float x = 0, y = 0, z = 0;
        float vx = 0, vy = 0, vz = 0;
        float age = 0, life = 1;
        float size0 = 0.3f;
        bool  spray = false;           // true = additive bow spray, else alpha foam
    };

    bool        m_built = false;
    FishSystem  m_fish;
    std::vector<Boat> m_boats;
    std::vector<Puff> m_puffs;         // fixed pool, round-robin reuse
    uint32_t    m_puffNext = 0;
    std::vector<x3::rhi::IRenderDevice::ParticleInstance> m_foamOut, m_sprayOut;
    x3::audio::SoundHandle m_outboardSnd{};
    float       m_waterY = 0.0f;       // the plan's waterY (== rendered plane)
    uint32_t    m_rng = 0x51CA7Eu;
};

} // namespace x3::game
