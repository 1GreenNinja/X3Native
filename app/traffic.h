#pragma once
// ---------------------------------------------------------------------------
// FREEWAY TRAFFIC — "now that we have a 16 lane freeway.. we will need to
// fill it with traffic ;->" (the owner, 2026-08-16).
//
// AI traffic for a dual-carriageway route (RoadSpec::dualCarriageway — the
// INNER TOUR). Nothing here invents road geometry: the lane centerlines are
// derived from the SAME machinery the pavement is built from —
// buildRoadRenderPath() fine stations + computeMedianPlan() offsets — so a
// traffic car sits on the painted lane by construction, at every median
// width, through every turnaround and junction. (A car that follows its lane
// spline can never wander into a junction's merge fillets: the fillets live
// outside the running lanes.)
//
// DIRECTION LAW (right-hand traffic, I-17 style): each carriageway drives so
// the MEDIAN is on the driver's LEFT.
//   * The route's fine stations carry a tangent T=(tx,tz); the ribbon's
//     lateral convention (road_network.cpp P()) is lat>0 == RIGHT of +u
//     travel == the (-tz,+tx) direction.
//   * The RIGHT carriageway (centre lat = +[median + kFwyPavedHalfM]) is
//     therefore driven in the +u direction: its driver's left (-lat) faces
//     the median at lat 0.  The LEFT carriageway is driven -u, same argument
//     mirrored. --test-traffic T1/T2 gate exactly this, geometrically.
//
// SIM: kinematic lane-followers, NOT Jolt vehicles. Position/heading advance
// along the lane spline at a per-vehicle cruise speed (55-90 mph by vehicle
// class; trucks keep right and slow, sports run the median lanes and fast),
// with a constant-time-gap following controller to the car ahead in the same
// lane (gap is NEVER negative — gated). Each live car owns a Jolt KINEMATIC
// box (IPhysicsWorld::addKinematicBox) marched via moveKinematic so the
// player's car can hit it and be shoved with real velocity; on a HARD impact
// (contact impulse over threshold) the body is converted DYNAMIC
// (makeBodyDynamic) and Jolt takes the wreck — the work-zone drum pattern,
// car-sized. Loose cars obey NO_SLOP rule 11 (the contact law): a per-frame
// clamp keeps them from ever sinking under the carved field.
//
// POPULATION: a deterministic (seeded xorshift) spawn/despawn ring around
// the player — spawn 300..1500 m out, cull beyond 1600 m, ~60 live cars.
//
// VISUALS: converted GLBs drawn per-drawable through drawMeshPBR (the
// DriveDemo skin path). Models are MEASURED at load with the CPU GLB reader
// (app/glb_cpu_read.h): bbox with the full node hierarchy applied. A model
// whose authored scale is off its real-car target length is uniformly
// rescaled to the documented target (rule 1: metres are law; the per-model
// target is authored data in kTrafficModels). A model that fails to load or
// measures degenerate is DROPPED loudly, never guessed at. Wheels: drawables
// under Wheel_* / wheel_* nodes spin about their own hub with road speed
// (radius = the hub's measured height — the asset's own number); models with
// no identifiable wheel nodes simply don't spin (rule: never fake it badly).
// ---------------------------------------------------------------------------
#include "road_network.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <memory>
#include <string>
#include <vector>

namespace x3::game {

struct TrafficConfig {
    uint32_t targetCount = 60;      // live cars maintained around the focus
    float    ringNearM   = 300.0f;  // spawn no closer than this
    float    ringFarM    = 1500.0f; // spawn no further than this
    float    cullM       = 1600.0f; // despawn beyond this
    uint32_t seed        = 0x7EAFF1Cu;
    // Contact impulse (approachSpeed * min mass, the engine's estimate) above
    // which a hit traffic car goes DYNAMIC. ~2.7 m/s closing at the hero
    // car's 1500 kg. Below it the kinematic body just shoves.
    float    looseImpulse = 4000.0f;
};

class FreewayTraffic {
public:
    // Build lane geometry + load the roster. `device` may be null (headless
    // self-test: no models, class-default car lengths). `phys` may be null
    // (pure-sim self-test: no bodies). Returns false (and builds nothing) for
    // a non-dual or degenerate spec.
    bool build(const RoadSpec& spec, const std::vector<float>& roadY,
               x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld* phys,
               std::string_view glbDir, const TrafficConfig& cfg = {});
    bool built() const { return m_built; }

    // Advance the sim + march the kinematic bodies toward their new poses.
    // Call BEFORE the host's phys->step() each frame. `focus` = player.
    void update(float dt, const float focus[3], x3::phys::IPhysicsWorld* phys);

    // Contact hook — forward from IPhysicsWorld::setContactCallback (the host
    // owns the one global callback). Converts a hard-hit car to dynamic.
    void onContact(x3::phys::BodyId a, x3::phys::BodyId b, float impulse,
                   x3::phys::IPhysicsWorld* phys);

    // Draw all live cars. Between beginFrame/endFrame.
    void render(const x3::rhi::FrameContext& frame, const float camPos[3]);

    void shutdown(x3::phys::IPhysicsWorld* phys);

    // ---- Queries (HUD / boot log / self-test) -----------------------------
    uint32_t liveCount() const { return (uint32_t)m_cars.size(); }
    uint32_t looseCount() const;
    uint32_t modelCount() const { return (uint32_t)m_models.size(); }
    const char* modelLabel(uint32_t i) const;

    // Per-car reads for the self-test (index into the live list).
    struct CarState {
        uint32_t id;          // stable spawn id (indices reorder on despawn)
        float x, y, z;        // ground-contact position (lane centre)
        float cx, cz;         // the ROUTE CENTRELINE sample at the same u —
                              // T2's median-on-left gate measures against it
        float dirX, dirZ;     // travel direction (unit, XZ)
        float v, cruise;      // current / cruise speed m/s
        int   cw, lane;       // carriageway 0=left(-lat) 1=right(+lat); lane 0=median side
        bool  loose;
        int   cls;            // TrafficClass
        float gapAhead;       // arc gap to the same-lane leader (1e9 = none)
    };
    CarState carState(uint32_t i) const;

    // Self-test hook: force-spawn a car at an exact station (bypasses the
    // ring/spacing rules). Returns live index or -1.
    int spawnForTest(int model, int cw, int lane, float s, float v, float cruise);

    // Route arc length of the lane path (travel-direction coordinate space).
    float routeLen() const { return m_totalLen; }

private:
    struct Model;
    struct Car;
    void   sampleAt(float u, float out[3], float dir[2], float* medianHalf,
                    float* dydu) const;
    float  uOfS(int cw, float s) const;
    bool   trySpawn(const float focus[3], x3::phys::IPhysicsWorld* phys);
    void   despawnCar(size_t idx, x3::phys::IPhysicsWorld* phys);
    uint32_t rnd();                       // xorshift32 (deterministic)
    float  rndf(float a, float b);

    bool m_built = false;
    TrafficConfig m_cfg;
    uint32_t m_rng = 1;

    // Lane path: the route's fine render stations (shared by both
    // carriageways; per-carriageway offsets are applied at sample time).
    std::vector<RoadRenderStation> m_path;
    float m_totalLen = 0.0f;
    bool  m_closed = false;

    // ---- Models -----------------------------------------------------------
    struct WheelGroup {
        std::vector<x3::asset::ModelDrawable> draw;
        float hub[3] = { 0, 0, 0 };   // model-space hub (from the node world matrix)
        float radius = 0.33f;         // measured: hub height above the origin plane
    };
    struct Model {
        std::string file, label;
        int   cls = 0;                // TrafficClass
        float mphMin = 60, mphMax = 75;
        int   laneMin = 1, laneMax = 6;
        int   weight = 10;
        float scale = 1.0f;           // measured-bbox -> target-length rescale
        float lenM = 4.5f, widthM = 1.85f, heightM = 1.45f;
        // -measuredMinY * scale: lifts the model so its own lowest point (the
        // tire contact patch) sits ON the lane — rule 11, a measured offset.
        float groundLift = 0.0f;
        bool  ok = false;
        std::unique_ptr<x3::asset::IAssetSource> src;
        std::unique_ptr<x3::asset::IModelLoader> loader;
        x3::asset::Model model;
        std::vector<x3::asset::ModelDrawable> body;
        std::vector<WheelGroup> wheels;
    };
    std::vector<Model> m_models;
    x3::rhi::IRenderDevice* m_device = nullptr;
    // For loose-car pose reads in render() — the RiverLife precedent (its
    // render also reads hull attitude straight from the physics world).
    x3::phys::IPhysicsWorld* m_phys = nullptr;

    // ---- Cars -------------------------------------------------------------
    struct Car {
        uint32_t id = 0;              // stable spawn id (see CarState::id)
        int   model = 0;
        int   cw = 1, lane = 4;
        float s = 0.0f;               // travel-direction arc position [0, L)
        float v = 25.0f, cruise = 30.0f;
        float spin = 0.0f;            // wheel angle (rad)
        float tint[3] = { 1, 1, 1 };  // clearcoat repaint (RCC models)
        bool  hasTint = false;
        bool  loose = false;
        x3::phys::BodyId body{};
        float halfH = 0.7f;           // body box half height (centre offset)
        float gapAhead = 1e9f;        // telemetry for the self-test
    };
    std::vector<Car> m_cars;
    uint32_t m_nextCarId = 1;
};

// --test-traffic: headless gates on the REAL inner course.
//   T1 no head-on traffic: every car's measured travel direction agrees with
//      its carriageway's law (right cw -> +u, left cw -> -u).
//   T2 the median is on every driver's LEFT (geometric, per car).
//   T3 following distance NEVER negative; a follower seeded on a slow leader
//      settles to a positive gap (positive control that the controller ran).
//   T4 the spawn/despawn ring: fill near a focus, counts in band, nothing
//      beyond the cull radius after the focus moves; deterministic (same
//      seed -> identical state hash).
//   T5 class discipline: heavy trucks only in the outer lanes.
bool runTrafficSelfTest();

} // namespace x3::game
