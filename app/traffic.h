#pragma once
// ---------------------------------------------------------------------------
// FREEWAY TRAFFIC — "now that we have a 16 lane freeway.. we will need to
// fill it with traffic ;->" (the owner, 2026-08-16), and then: "Traffic!!!!
// However... we need some that honks.. some that accelerates... a radar speed
// sign... cops... and some different colors on the cars.. Also.. some
// different performance profiles on the cars and trucks.. Oh and switch
// lanes!" + "Throw in some random jerk drivers..." + "and some cars that
// break down.. and have to call a tow truck" (2026-08-17).
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
// THE LANE COORDINATE IS CONTINUOUS (W-TRAFFIC2). `Car::laneF` is a FLOAT lane
// index, and laneLat() is linear in it, so:
//   * an integer laneF is a lane centre (0 = median-side fast lane,
//     kFwyLaneCount-1 = the outer lane),
//   * a fractional laneF is a car MID-MERGE, riding a smoothstep between two
//     lane centres over ~2-3 s,
//   * laneF > kFwyLaneCount-1 leaves the running lanes for the paved
//     shoulder/apron — which is where a BREAKDOWN parks and where the tow
//     truck drives.
//   This one generalization is also the whole reason a future RAMP is a spec
//   and not a rewrite: a ramp is a lane whose lateral offset diverges from the
//   mainline over distance. See "RAMPS — WHAT WOULD BE NEEDED" at the bottom.
//
// SIM: kinematic lane-followers, NOT Jolt vehicles. Position/heading advance
// along the lane spline at a per-vehicle cruise speed, with a constant-time-gap
// following controller to the car ahead (gap is NEVER negative — gated) and a
// hard no-overlap pass that works in 2-D LATERAL TERMS, so a car mid-merge can
// no more occupy another car's space than a car in-lane can. Each live car owns
// a Jolt KINEMATIC box (IPhysicsWorld::addKinematicBox) marched via
// moveKinematic so the player's car can hit it and be shoved with real
// velocity; on a HARD impact (contact impulse over threshold) the body is
// converted DYNAMIC (makeBodyDynamic) and Jolt takes the wreck — the work-zone
// drum pattern, car-sized. Loose cars obey NO_SLOP rule 11 (the contact law): a
// per-frame clamp keeps them from ever sinking under the carved field.
//
// DRIVERS: every car draws a CLASS profile (cruise band, accel, brake, lane
// preference — a truck is slow to spool AND slow to stop; a sports car cruises
// faster and accelerates hard) and a TEMPERAMENT (normal / aggressive / one of
// five flavours of jerk). Temperament scales headway, overtake urge, signal
// discipline and gap tolerance. Every one of them, jerks included, is still
// subject to the no-overlap pass: a jerk cuts it CLOSE, never clips.
//
// POPULATION: a deterministic (seeded xorshift) spawn/despawn ring around
// the player — spawn 300..1500 m out, cull beyond 1600 m, ~300 live cars.
//
// VISUALS: converted GLBs drawn per-drawable through drawMeshPBR (the
// DriveDemo skin path). Models are MEASURED at load with the CPU GLB reader
// (app/glb_cpu_read.h): bbox with the full node hierarchy applied. A model
// whose authored scale is off its real-car target length is uniformly
// rescaled to the documented target (rule 1: metres are law; the per-model
// target is authored data in kTrafficModels). A model that fails to load or
// measures degenerate is DROPPED loudly, never guessed at. Wheels: drawables
// under Wheel_* / wheel_* / *Tire* nodes spin about their own hub with road
// speed (radius = the hub's measured height — the asset's own number).
//
// FURNITURE built procedurally in this file (there is no police car, no tow
// truck and no radar sign anywhere in the 914-package catalog — see the
// receipts in traffic.cpp): the cop LIGHT BAR, the tow truck's AMBER BEACONS
// and TOWBOOK wordmark, the breakdown HAZARD lamps, and the ROADSIDE RADAR
// SPEED SIGN. All of them bake their emissive through lns::makeSignRGBA-style
// texture gating over a near-black albedo (X3_WORLD_RULES rule 5 / rule 7),
// never a flat >0.5 emissive factor.
// ---------------------------------------------------------------------------
#include "road_network.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/audio/IAudioSystem.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <memory>
#include <string>
#include <vector>

namespace x3::game {

struct TrafficConfig {
    // POPULATION. 60 (the first pass) left a SIXTEEN-lane freeway reading as
    // deserted — measured from the air at station u=20778: one car per ~400 m
    // of road spread across 16 lanes. The owner's ask was "fill it with
    // traffic". 300 puts roughly one vehicle per 100 lane-metres inside the
    // ring, which is real light-to-moderate freeway flow; the same-lane
    // spacing rule settles the live count around 300-370 depending on how
    // much route falls inside the ring. Receipts: shots_traffic/03 (200),
    // 04 (400) and the [tunnel-perf] lines beside them.
    uint32_t targetCount = 300;     // live cars maintained around the focus
    float    ringNearM   = 300.0f;  // spawn no closer than this
    float    ringFarM    = 1500.0f; // spawn no further than this
    float    cullM       = 1600.0f; // despawn beyond this
    uint32_t seed        = 0x7EAFF1Cu;
    // Contact impulse (approachSpeed * min mass, the engine's estimate) above
    // which a hit traffic car goes DYNAMIC. ~2.7 m/s closing at the hero
    // car's 1500 kg. Below it the kinematic body just shoves.
    float    looseImpulse = 4000.0f;
    // X3_TRAFFIC_NEAR/_FAR/_COUNT are read in build() when this is true (the
    // host's path). --test-traffic sets it FALSE: a gate that moves because
    // someone left a capture lever exported in their shell is not a gate.
    bool     envOverrides = true;

    // ---- W-TRAFFIC2 character dials (all fractions of the live population) --
    // A MINORITY, deliberately: the freeway must read as normal traffic with
    // jerks in it, not as chaos. --test-traffic T7 gates these bands.
    float    aggressiveFrac = 0.14f;  // hard on the gas, overtakes readily
    float    jerkFrac       = 0.07f;  // the antisocial five (see JerkKind)
    float    copFrac        = 0.012f; // ~3-4 patrol cars in a 300-car ring
    // Mean seconds between BREAKDOWN events anywhere in the live population.
    // A handful per session, never a parade of dead cars.
    float    breakdownMeanS = 150.0f;
    // Speed the radar sign flashes above, mph. The owner's freeway has no
    // posted limit yet; 70 is the I-17 number and the sign is the only place
    // in the world that states one, so this IS the posted limit until a
    // signage pass says otherwise (NO_SLOP rule 4: if a speed-limit sign is
    // ever added, it and this are ONE value).
    float    radarLimitMph  = 70.0f;
};

// The five flavours of jerk. Each car gets exactly ONE, because a driver who
// does all five reads as a physics bug rather than as a person. Every one of
// them has to be LEGIBLE from the driver's seat — that is the whole point.
enum class JerkKind : uint8_t {
    None = 0,
    Cutter,        // merges into gaps far too small. Gets honked AT.
    LaneHog,       // sits in lane 0/1 UNDER the flow and will not move over.
    Weaver,        // ~30 mph over, changes lane at the first excuse.
    BrakeChecker,  // when tailgated, stabs the brakes.
    Tailgater,     // closes right up (on the PLAYER by preference), then swerves by.
    Count
};

class FreewayTraffic {
public:
    // Build lane geometry + load the roster. `device` may be null (headless
    // self-test: no models, class-default car lengths). `phys` may be null
    // (pure-sim self-test: no bodies). `audio` may be null (silent sim — the
    // horns/siren simply never play; every gate still runs). Returns false
    // (and builds nothing) for a non-dual or degenerate spec.
    bool build(const RoadSpec& spec, const std::vector<float>& roadY,
               x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld* phys,
               std::string_view glbDir, const TrafficConfig& cfg = {},
               x3::audio::IAudioSystem* audio = nullptr);
    bool built() const { return m_built; }

    // Advance the sim + march the kinematic bodies toward their new poses.
    // Call BEFORE the host's phys->step() each frame. `focus` = player.
    void update(float dt, const float focus[3], x3::phys::IPhysicsWorld* phys);

    // THE PLAYER, for the two systems that need more than his position:
    //   * the RADAR SIGN reads `speedMps` and displays it in mph;
    //   * the TAILGATER jerk picks him over an AI car when he is in reach.
    // Call once per frame before update(). Never required — with no call the
    // sign reads 0 and tailgaters pick AI victims (the self-test's path).
    void setPlayer(const float pos[3], float speedMps);

    // Contact hook — forward from IPhysicsWorld::setContactCallback (the host
    // owns the one global callback). Converts a hard-hit car to dynamic.
    void onContact(x3::phys::BodyId a, x3::phys::BodyId b, float impulse,
                   x3::phys::IPhysicsWorld* phys);

    // Draw all live cars + the roadside furniture. Between beginFrame/endFrame.
    void render(const x3::rhi::FrameContext& frame, const float camPos[3]);

    // The point lights traffic wants THIS frame: cop light bars, tow beacons,
    // breakdown hazards, and the radar sign's panel wash. Bounded (see
    // kMaxTrafficLights) and sorted nearest-first, so a host merging them into
    // its own array can truncate from the back without losing the close ones.
    // The host owns the ONE setPointLights call — this is its contribution.
    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }

    void shutdown(x3::phys::IPhysicsWorld* phys);

    // ---- Queries (HUD / boot log / self-test) -----------------------------
    uint32_t liveCount() const { return (uint32_t)m_cars.size(); }
    uint32_t looseCount() const;
    uint32_t modelCount() const { return (uint32_t)m_models.size(); }
    const char* modelLabel(uint32_t i) const;
    // Rolling counters for the HUD line and the gates.
    uint32_t hornCount()      const { return m_hornCount; }
    uint32_t laneChangeCount() const { return m_laneChanges; }
    uint32_t mergingCount()   const;
    uint32_t copCount()       const;
    uint32_t jerkCount()      const;
    uint32_t breakdownCount() const;
    // The radar sign's CURRENT reading (mph, as displayed) and whether it is
    // flashing. -1 = no sign sited / nothing to show.
    int   radarReadingMph() const { return m_radar.shownMph; }
    bool  radarFlashing()   const { return m_radar.over; }
    bool  radarSited()      const { return m_radar.sited; }
    // Where the sign STANDS, so a gate can put the player in front of it
    // rather than guessing a coordinate (gotcha 4.1's law, applied to a test).
    void  radarProbePos(float out[3]) const;

    // Per-car reads for the self-test (index into the live list).
    struct CarState {
        uint32_t id;          // stable spawn id (indices reorder on despawn)
        float x, y, z;        // ground-contact position (lane centre)
        float cx, cz;         // the ROUTE CENTRELINE sample at the same u —
                              // T2's median-on-left gate measures against it
        float dirX, dirZ;     // travel direction (unit, XZ)
        float v, cruise;      // current / cruise speed m/s
        int   cw;             // carriageway 0=left(-lat) 1=right(+lat)
        int   lane;           // NEAREST lane index (round of laneF)
        float laneF;          // CONTINUOUS lane coordinate (see the header)
        float lat;            // signed lateral offset from the route centreline
        float halfWidth;      // this model's half width, m
        bool  loose;
        int   cls;            // TrafficClass
        float gapAhead;       // arc gap to the leader in reach (1e9 = none)
        bool  merging;        // mid lane change
        bool  brakeLit;       // brake lights on (decel past the threshold)
        bool  blockedByPlayer;// the thing I am following IS the player
        int   signalDir;      // -1 signalling left, 0 none, +1 right
        int   temper;         // 0 normal, 1 aggressive, 2 jerk
        int   jerk;           // JerkKind
        int   role;           // CarRole (0 civilian, 1 cop, 2 tow, 3 broken)
        float s;              // travel-direction arc position
    };
    CarState carState(uint32_t i) const;

    // Self-test hook: force-spawn a car at an exact station (bypasses the
    // ring/spacing rules). Returns live index or -1.
    int spawnForTest(int model, int cw, int lane, float s, float v, float cruise);
    // Like spawnForTest but ALSO creates the Jolt kinematic body, so a gate can
    // exercise the collision path (--test-traffic T12). Every other gate runs
    // with phys = nullptr, which is precisely how a physics defect stayed
    // invisible behind twenty green checks.
    int  spawnForTestPhys(int model, int cw, int lane, float s, float v,
                          float cruise, x3::phys::IPhysicsWorld* phys);
    bool carHasBody(uint32_t i) const;
    x3::phys::BodyId carBodyId(uint32_t i) const;
    // The PHYSICS box (centre + half extents) and the box the RENDERER would
    // draw the model into, both in world space. If these disagree you can
    // collide with nothing where the car appears — the "no collision factor"
    // defect's most likely shape, so it is measured, not assumed.
    void carBodyBox(uint32_t i, float outCentre[3], float outHalf[3]) const;
    void carDrawnBox(uint32_t i, float outLo[3], float outHi[3]) const;

    // Self-test hooks for the character systems: force a temperament / a role
    // onto a live car so a gate can drive ONE behaviour deterministically
    // instead of waiting for the dice.
    bool setTemperForTest(uint32_t idx, int temper, JerkKind jerk);
    bool forceBreakdownForTest(uint32_t idx);

    // Route arc length of the lane path (travel-direction coordinate space).
    float routeLen() const { return m_totalLen; }

    // The paved half-width available OUTSIDE the running lanes, per side, m.
    // The breakdown gate measures against this.
    static float shoulderRoomM() { return kFwyPavedHalfM - kFwyRunningHalfM; }

    // World position of a lane point — so a caller (or a gate) can PARK
    // something exactly on a lane instead of guessing a coordinate.
    bool laneWorldPos(int cw, float laneF, float s, float out[3]) const;
    // Where the sim currently thinks the player is, in ITS coordinates.
    // Returns false until setPlayer has been called and he is near the road.
    bool playerLane(int& cw, float& laneF, float& s, float& v) const;

private:
    struct Model;
    struct Car;
    struct SortIdx;
    void   sampleAt(float u, float out[3], float dir[2], float* medianHalf,
                    float* dydu) const;
    float  uOfS(int cw, float s) const;
    bool   trySpawn(const float focus[3], x3::phys::IPhysicsWorld* phys);
    void   despawnCar(size_t idx, x3::phys::IPhysicsWorld* phys);
    uint32_t rnd();                       // xorshift32 (deterministic)
    float  rndf(float a, float b);
    void   logCameraStations() const;
    // After an X3_TRAFFIC_PRESIM fast-forward: print a paste-ready --shot-cam
    // AIMED AT each thing worth photographing (a lit patrol car, a car
    // mid-merge, the breakdown, the tow, the sign). Gotcha 4.1's "derive
    // cameras from the data" law, applied to subjects that MOVE.
    void   reportShotCams() const;
    // --- W-TRAFFIC2 sim stages, in the order update() runs them -------------
    void   rebuildOrder();                                  // sorted-by-s index
    void   driveFollowers(float dt);                        // long. control
    void   thinkLaneChanges(float dt);                      // merge decisions
    void   advanceMerges(float dt);                         // lateral spline
    void   enforceNoOverlap();                              // the hard invariant
    void   runRoles(float dt, x3::phys::IPhysicsWorld* phys); // cops/breakdown/tow
    void   serviceHorns(float dt);
    // --- per-car construction ----------------------------------------------
    void   giveCharacter(Car& c);       // class profile + temperament
    void   givePaint(Car& c);           // the curated palette draw
    int    modelIndexByFile(const char* file) const;
    void   beginBreakdown(Car& c);
    void   spawnTowFor(Car& job, x3::phys::IPhysicsWorld* phys);
    void   worldPosOf(const Car& c, float out[3]) const;
    // THE PURSUIT SEAM. A full pursuit AI is NOT in scope for this pass (see
    // the note at the definition); this is called once, the moment a patrol
    // car lights up, and is where that behaviour would attach.
    void   onCopWouldPursue(Car& cop);
    // --- geometry helpers ---------------------------------------------------
    float  crabYaw(const Car& c) const;                     // mid-merge heading
    float  arcDelta(float sa, float sb) const;              // signed, shortest
    bool   latOverlap(const Car& a, const Car& b, float slack) const;
    void   buildFurniture(x3::rhi::IRenderDevice& dev, std::string_view glbDir);
    // The bits bolted ONTO a car (cop bar, tow beacons + TOWBOOK, hazards) and
    // the roadside sign. Split out of render() so the per-car loop stays the
    // shape it has always had.
    void   renderCarFurniture(const x3::rhi::FrameContext& frame, const Car& c,
                              const float r[3], const float u[3], const float f[3],
                              float originY);
    void   renderRadarSign(const x3::rhi::FrameContext& frame);
    void   siteRadarSign();
    void   updateRadar(float dt);
    void   collectLights();
    // Lateral half-extent a car sweeps, and the arc gap two cars need.
    float  carHalfWidth(const Car& c) const;
    // Is `laneF` a legal place for this car to sit? (Running lanes, plus the
    // shoulder band for the roles allowed onto it.)
    bool   laneAllowed(const Car& c, float laneF) const;
    // Measured safety check for a merge: true when `target` is clear for `c`.
    bool   laneGapSafe(size_t ci, float targetLane, float scale) const;
    void   startMerge(Car& c, float targetLane, float lead);
    void   honk(Car& c, const char* why);

    bool m_built = false;
    TrafficConfig m_cfg;
    uint32_t m_rng = 1;
    float m_time = 0.0f;
    // X3_TRAFFIC_PRESIM seconds still owed, burned on the first update().
    float m_presimS = 0.0f;

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
        // PAINTABLE, per drawable, decided ONCE at load: a drawable takes the
        // per-instance paint only if it is a clearcoat paint panel or an
        // untextured light-coloured body factor. Deciding it per FRAME (the
        // first cut) meant re-testing 9 conditions on every drawable of every
        // one of 300 cars, and deciding it per MODEL meant the armory cars'
        // textured shells got repainted into flat blobs. Parallel to `body`.
        std::vector<uint8_t> bodyPaintable;
        std::vector<WheelGroup> wheels;
    };
    std::vector<Model> m_models;
    x3::rhi::IRenderDevice*  m_device = nullptr;
    x3::phys::IPhysicsWorld* m_phys = nullptr;
    x3::audio::IAudioSystem* m_audio = nullptr;

    // ---- Cars -------------------------------------------------------------
    enum CarRole : uint8_t { RoleCivilian = 0, RoleCop, RoleTow, RoleBroken };
    enum Temper  : uint8_t { TempNormal = 0, TempAggressive, TempJerk };
    struct Car {
        uint32_t id = 0;              // stable spawn id (see CarState::id)
        int   model = 0;
        int   cw = 1;
        // THE CONTINUOUS LANE COORDINATE. `laneF` is where the car IS;
        // `laneFrom`/`laneTo` bracket an in-progress merge and `mergeT` runs
        // 0..1 across it on a smoothstep. Not merging => laneFrom==laneTo==laneF.
        float laneF = 4.0f, laneFrom = 4.0f, laneTo = 4.0f;
        float mergeT = 1.0f, mergeDur = 2.4f;
        float signalT = 0.0f;         // >0: signalling, counting down to the merge
        int   signalDir = 0;          // -1 left (toward median), +1 right
        float thinkT = 0.0f;          // next lane-change deliberation, s
        float s = 0.0f;               // travel-direction arc position [0, L)
        float v = 25.0f, cruise = 30.0f;
        float spin = 0.0f;            // accumulated -distance; theta = spin/radius
        float tint[3] = { 1, 1, 1 };  // per-instance paint
        bool  hasTint = false;
        bool  loose = false;
        x3::phys::BodyId body{};
        float halfH = 0.7f;           // body box half height (centre offset)
        float gapAhead = 1e9f;        // telemetry for the self-test
        float leaderV = 0.0f;         // the speed of whoever I am following
        float lastAccel = 0.0f;       // signed m/s^2 actually applied this tick
        bool  brakeLit = false;       // rear lamps lit (see kBrakeLightMps2)
        bool  blockedByPlayer = false;// my leader this tick IS the player
        bool  parked = false;         // breakdown: fully stopped on the shoulder
        // >=0 caps this car's target speed from the ROLE stage (a tow closing
        // on a job, a cop slowing at the scene). -1 = the role has no opinion.
        float roleSpeedOverride = -1.0f;
        float accelMax = 2.5f, brakeMax = 6.5f, headway = 1.6f, minGap = 7.0f;
        float overtakeUrge = 0.35f;
        int   prefLaneMin = 1, prefLaneMax = 6;
        uint8_t temper = TempNormal;
        JerkKind jerk = JerkKind::None;
        uint8_t role = RoleCivilian;
        // Horn / annoyance bookkeeping.
        float hornCooldown = 0.0f;
        float tailgatedFor = 0.0f;    // seconds someone has been on MY bumper
        float tailgatingFor = 0.0f;   // seconds I have been on SOMEONE ELSE'S
        float lastV = 0.0f;           // for the hard-brake detector
        float brakeCheckT = 0.0f;     // BrakeChecker: stab timer
        // Role state (cop lights / breakdown / tow), all in seconds.
        float roleT = 0.0f;           // generic phase timer for the role
        float roleNext = 60.0f;       // when the role's next transition is due
        bool  lightsOn = false;       // cop: running code / tow: beacons
        x3::audio::LoopHandle siren{};
        uint32_t towTarget = 0;       // tow: the broken car's id (0 = none)
        float towCalled = -1.0f;      // breakdown: seconds until the tow rolls
        bool  hooked = false;         // tow: on scene, winching
        float hookDur = 12.0f;        // tow: seconds on scene before clearing
        float phase = 0.0f;           // per-car flash phase so lights don't sync
    };
    std::vector<Car> m_cars;
    uint32_t m_nextCarId = 1;

    // Per-carriageway car indices sorted by s, rebuilt once per update. Every
    // O(n^2) scan in the first pass (follower, overlap) became a bounded walk
    // over this — with 300 cars the old all-pairs loops were 90k iterations
    // EACH and the three new scans this pass adds would have tripled that.
    std::vector<uint32_t> m_order[2];

    // ---- The player -------------------------------------------------------
    float m_focus[3] = { 0, 0, 0 };   // this frame's focus (the update arg)
    float m_playerPos[3] = { 0, 0, 0 };
    float m_prevPlayerPos[3] = { 0, 0, 0 };
    float m_playerSpeed = 0.0f;
    bool  m_havePlayer = false;

    // THE PLAYER AS AN OBSTACLE. The owner parked on the freeway and watched
    // traffic drive straight through him: the following controller resolved
    // against AI cars only, so a stopped player was not in the world model at
    // all — nobody braked, nobody merged, nobody honked, and the only thing
    // that fired was the kinematic->dynamic conversion at impulse 4000, which
    // is physics working perfectly on a collision that should never have
    // happened. This projects him onto the same (cw, s, laneF) coordinates
    // every AI car lives in, so the follower, the merge test, the no-overlap
    // pass and the horns can treat him as exactly what he is: a vehicle.
    struct Obstacle {
        bool  valid = false;
        int   cw = 1;
        float s = 0.0f;
        float laneF = 0.0f;
        float v = 0.0f;        // SIGNED along this carriageway's travel
        float halfW = 0.95f;
        float lenM = 4.6f;
        float lat = 0.0f;      // signed lateral offset from the route centreline
        bool  inLane = false;  // inside the running lanes (not on the verge)
    };
    Obstacle m_player;
    void projectPlayer(float dt);
    // X3_TRAFFIC_PARK — a virtual stopped vehicle for captures (see build()).
    bool  m_parked = false, m_parkLogged = false;
    int   m_parkCw = 1;
    float m_parkLane = 5.0f, m_parkS = 0.0f;

    // ---- Horn budget ------------------------------------------------------
    // A jam must not be a cacophony: at most one horn every kHornGlobalGapS
    // across the WHOLE freeway, and each car has its own long cooldown.
    float m_hornGlobalCd = 0.0f;
    uint32_t m_hornCount = 0;
    uint32_t m_laneChanges = 0;
    float m_breakdownCd = 0.0f;

    // ---- Sounds -----------------------------------------------------------
    x3::audio::SoundHandle m_sndHornCar{}, m_sndHornTruck{}, m_sndSiren{};

    // ---- Furniture (built once, drawn per instance) -----------------------
    // Nothing here is a scene Entity: like the traffic cars themselves these
    // are direct draws, so no streamed-region ownership ledger can capture
    // them (world_cars.h documents why that matters).

    // A loaded GLB prop. The cop LIGHT BAR is a real authored asset (RCC v4's
    // Model_Police_Siren, converted by tools/convert_lightbar_glb.py), not a
    // box: its origin is its MOUNT FACE and its red/blue lens primitives are
    // identified by the emissiveFactor the converter baked in — (1,0,0) red,
    // (0,0,1) blue, (0,0,0) housing. See that script's header for why the
    // identity rides emissiveFactor and not a material name.
    struct GlbProp {
        std::unique_ptr<x3::asset::IAssetSource> src;
        std::unique_ptr<x3::asset::IModelLoader> loader;
        x3::asset::Model model;
        std::vector<x3::asset::ModelDrawable> draw;
        std::vector<uint8_t> lens;    // per drawable: 0 housing, 1 red, 2 blue
        float w = 0, h = 0, d = 0;    // measured extents, m
        bool  ok = false;
    };
    GlbProp m_lightBar;

    // A procedural mesh + its material set.
    struct Prop {
        x3::rhi::MeshHandle mesh{};
        x3::rhi::TextureHandle base{}, mr{}, emis{};
        uint32_t tris = 0;
    };
    Prop m_lens;        // small amber dome: tow beacons AND breakdown hazards
    Prop m_steel;       // radar sign post + housing (one welded mesh)
    Prop m_glass;       // radar sign dark-glass face (rule 7)
    Prop m_plate;       // ONE two-faced 0..1-UV quad, both faces readable
    Prop m_quad;        // ONE one-faced 0..1-UV quad (sign header + digits)
    // Wordmark textures drawn on m_plate. TOWBOOK is the owner's own company.
    x3::rhi::TextureHandle m_texTowbook{}, m_texPolice{}, m_texHeader{};
    x3::rhi::TextureHandle m_texDigit[11]{};   // '0'..'9' then BLANK
    x3::rhi::TextureHandle m_texWhite{}, m_texMrDull{}, m_texMrLens{};
    bool m_furniture = false;

    // ---- The radar sign ---------------------------------------------------
    struct Radar {
        bool  sited = false;
        float pos[3] = { 0, 0, 0 };     // post base, on the shoulder
        float dirX = 1.0f, dirZ = 0.0f; // the way the traffic it reads travels
        float u = 0.0f;
        int   shownMph = -1;
        bool  over = false;
        float flashPhase = 0.0f;
        float holdT = 0.0f;             // holds the last reading briefly
    } m_radar;

    std::vector<x3::rhi::PointLight> m_lights;
};

// --test-traffic: headless gates on the REAL inner course.
//   T1 no head-on traffic: every car's measured travel direction agrees with
//      its carriageway's law (right cw -> +u, left cw -> -u).
//   T2 the median is on every driver's LEFT (geometric, per car).
//   T3 following distance NEVER negative; a follower seeded on a slow leader
//      settles to a positive gap (positive control that the controller ran).
//   T4 the spawn/despawn ring: fill near a focus, counts in band, nothing
//      beyond the cull radius after the focus moves; deterministic.
//   T5 class discipline: heavy trucks keep to the outer lanes.
//   T6 LANE CHANGES happen, and NO TWO CARS EVER OVERLAP — measured in 2-D
//      (arc gap AND lateral gap), every tick, including mid-merge.
//   T7 the character mix: profiles are measurably distinct (sports cruise
//      faster and accelerate harder than trucks), and the aggressive/jerk/cop
//      fractions land in their configured bands.
//   T8 HORNS are rate-limited (a jam is not a cacophony) and sirens bounded.
//   T9 BREAKDOWNS end fully on the shoulder (measured lateral clearance from
//      the running lanes), never in a live lane; the tow reaches them; both
//      despawn with no leak in the car pool.
//   T10 the RADAR SIGN reads the player's speed in mph and flashes over limit.
bool runTrafficSelfTest();

// ---------------------------------------------------------------------------
// RAMPS — WHAT WOULD BE NEEDED (the Stack interchange lane's future spec).
// NOT built here, deliberately: the sibling lane owns the interchange geometry
// and routing onto it before that geometry exists would be guesswork. What
// this file already provides, and what a ramp pass would still have to add:
//
//   PROVIDED. (a) A continuous float lane coordinate whose lateral offset is a
//   pure function laneLat(cw, laneF, medianHalf) — a ramp is exactly a lane
//   whose offset leaves that function's range over an s interval. (b) A merge
//   controller that already moves a car between two lateral offsets on a
//   timed smoothstep with a measured both-directions gap check. (c) A
//   no-overlap pass that is lateral-aware, so a car merging off the mainline
//   cannot clip one staying on it. (d) Roles that already leave the running
//   lanes (breakdown/tow on the shoulder) and come back.
//
//   STILL NEEDED. (1) A ROUTE GRAPH: today a car's whole itinerary is
//   (cw, s increasing) on ONE spline. A ramp needs each car to carry a route —
//   a list of (path, s0, s1) edges — and sampleAt() to dispatch on the car's
//   current edge rather than on m_path alone. (2) RAMP SPLINES from the
//   interchange builder in the same RoadRenderStation form m_path uses, with
//   the gore point (where the ramp's lateral offset first differs from the
//   mainline's) and the nose (where it is fully separate) marked, so the merge
//   controller has an s window to complete in. (3) A DECISION at each gore:
//   per car, exit or stay — weighted, deterministic, decided far enough
//   upstream that a truck in lane 7 can legally reach lane 8 in time (this is
//   the one that makes bad ramp AI look bad: last-second lane dives). (4)
//   PRIORITY at the merge: a car ON the mainline has right of way over one
//   entering, so the entering car's target speed must additionally track a gap
//   in the DESTINATION lane, not just its own. (5) The no-overlap pass must
//   compare cars across DIFFERENT paths near a junction — today "same cw" is a
//   sufficient partition and it would stop being one. (6) The ring spawner
//   must not spawn inside a gore.
// ---------------------------------------------------------------------------

} // namespace x3::game
