#pragma once
// WORLD CARS — findable, drivable, hackable vehicles in the ONE WORLD
// (canonlevel). Game/slice code only — engine/ stays pure.
//
// DESIGN (this wave is WIRING, not vehicle R&D — the physics is the existing
// engine/physics/IVehicle.h wheeled framework via app/vehicle.h's DriveDemo):
//   * PARKED cars are an authored set (WorldCarDef list). A parked car is a
//     DIRECT-DRAW visual (the hero-car CTR GLB drawn per frame at the parked
//     pose, per-car paint tint; graybox box+wheels fallback without the GLB)
//     plus ONE static Jolt box so the player collides with it. Parked cars are
//     NOT scene entities on purpose: the streamed-region ownership ledgers
//     capture every Scene::add() in their realize window and destroy the
//     captured meshes at evict — a shared GLB mesh must never land in a region
//     ledger. Region-owned cars are therefore wired through onRegionBuild /
//     onRegionTeardown (forwarded from WorldStreamer::setRegionHooks): the
//     hooks only add/remove OUR OWN static bodies + flip residency, so region
//     stream-out/in cycles are leak-free by construction and the
//     --test-worldstream created==destroyed invariants are untouched.
//   * ONE live vehicle, SPAWNED ON FIRST ENTRY (cheaper: zero Jolt
//     VehicleConstraints exist until the player actually drives; safer: no
//     fleet of dormant constraints to wake/leak). Entering teleports the
//     single DriveDemo to the parked pose; exiting re-parks the visual at the
//     car's rest pose and returns the live rig to a limbo slab far below the
//     world (y -600, under the club/strata band) where it idles on handbrake.
//   * ENTER (Riftforged precedent): E within reach (~2.6 m of the body) takes
//     the wheel — the player capsule is STASHED (parked high above the car;
//     Player::update is skipped while driving so it neither falls nor
//     collides), WASD throttle/steer + Space brake map to the
//     IVehicleController (host_drive's mapping), the camera goes to the drive
//     host's chase framing. E (or F) exits: the car brakes to rest, the player
//     capsule is restored at the door side ON the terrain (placeOnTerrain
//     contract via the injected ground query; deep water on that side flips to
//     the other door).
//   * HACKABLE: locked cars prompt "LOCKED — [hold E] hack". Holding E for
//     kHackSeconds unlocks PERMANENTLY (a host-side unlocked-id set survives
//     region rebuilds, so hacking isn't Sisyphean) and fires the alarm hook
//     (the host wires facilityAlert.reportTerminalHack — guarded exactly like
//     the keypad path — plus crowd onViolence scatter) + a buzz/chime alarm
//     chirp. Releasing E early resets progress.
//   * WATER: driving into deep river/sea water kills the engine and forces an
//     exit-swim (the host calls forceExit; the restored capsule's own water
//     query puts the player straight into the W10 swim state).
//
// Headless-testable: build() with a null device skips all visuals; the ground/
// water queries are injected (the canon host passes placeOnTerrain /
// worldWaterLevelAt; the self-test injects a flat slab) — mirrors
// Player::setWaterQuery. See runCanonVehicleSelfTest (--test-canonvehicle).

#include "vehicle.h"
#include "player.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/audio/IAudioSystem.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace x3::game {

// Hold-E hack duration (s) + interaction reach (m, XZ to the parked body).
constexpr float kCarHackSeconds = 3.0f;
constexpr float kCarReach       = 2.6f;

// One authored parked car.
struct WorldCarDef {
    std::string id;                 // stable id (the unlocked-set key)
    float x = 0.0f, z = 0.0f;       // authored XZ (Y from the ground query)
    float yawDeg = 0.0f;            // parked nose direction: +Z rotated yawDeg about +Y
    bool  locked = false;           // locked => hold-E hack before entry
    float tint[3] = { 1, 1, 1 };    // paint color (replaces the clearcoat coat)
    std::string region;             // "" = host-owned (resident from boot);
                                    // else the WorldStreamer region id that owns it
};

class WorldCars {
public:
    using GroundFn = std::function<float(float x, float z)>;  // surface Y at (x,z)
    // Water SURFACE Y over (x,z) or a very-negative dry sentinel (terrain.h's
    // kWorldWaterDry). Unset => the world is dry (self-tests, dev worlds).
    using WaterFn  = std::function<float(float x, float z)>;
    using AlarmFn  = std::function<void(const x3::phys::Vec3& pos)>;

    // Build the authored set. `device` may be null (headless: no GLB/meshes —
    // physics + logic only). Host-owned cars ("" region) become resident now;
    // region cars wait for onRegionBuild. Ground query REQUIRED before build.
    bool build(const std::vector<WorldCarDef>& defs,
               x3::rhi::IRenderDevice* device, x3::phys::IPhysicsWorld& physics,
               std::string_view glbDir);
    bool built() const { return m_built; }
    double bootMs() const { return m_bootMs; }

    void setGroundQuery(GroundFn fn) { m_ground = std::move(fn); }
    void setWaterQuery(WaterFn fn)   { m_water  = std::move(fn); }
    // Fired ONCE per successful hack, at the car position (host: guarded
    // facilityAlert.reportTerminalHack + crowd onViolence scatter).
    void setHackAlarmHook(AlarmFn fn) { m_alarm = std::move(fn); }

    // ---- Region lifecycle (forward from WorldStreamer::setRegionHooks) ----
    // onRegionBuild: cars owned by `regionId` (re)park at their AUTHORED pose
    // (fresh parked state on every re-realize — v1 policy; the unlocked-id set
    // is host-side and survives). onRegionTeardown removes their static bodies
    // (a car currently being DRIVEN is the host-owned live rig — untouched).
    void onRegionBuild(std::string_view regionId, x3::phys::IPhysicsWorld& physics);
    void onRegionTeardown(std::string_view regionId, x3::phys::IPhysicsWorld& physics);

    // ---- Per-frame interaction (call once per render frame) ----------------
    // playerFeet: the on-foot feet position (ignored while driving). eHeld =
    // E currently down; eEdge = E rising edge; exitEdge = an additional exit
    // edge (F). Updates the prompt line + the hold-E hack timer; performs
    // enter/exit. `player` may be null (screenshot staging / tests without a
    // capsule). Returns true iff a vehicle consumed this frame's E press.
    bool interact(const x3::phys::Vec3& playerFeet, bool eHeld, bool eEdge,
                  bool exitEdge, float dt, Player* player,
                  x3::phys::IPhysicsWorld& physics, x3::audio::IAudioSystem* audio);
    // The HUD hint line ("[E] Enter" / "LOCKED — [hold E] hack" /
    // "HACKING... 47%" / "[E] Exit"). Empty when nothing is in reach.
    const std::string& prompt() const { return m_prompt; }

    // ---- Driving ------------------------------------------------------------
    bool driving() const { return m_driving; }
    int  drivenIndex() const { return m_drivenIdx; }
    // host_drive input mapping (WASD throttle/steer, Space handbrake; the host
    // adds the reverse->brake rule). setInput routes through DriveDemo's TC.
    void driveInput(const x3::phys::VehicleInput& in) { if (m_driveBuilt) m_drive.setInput(in); }
    void preStep(float dt)  { if (m_driveBuilt) m_drive.preStep(dt); }
    void postStep(float dt) { if (m_driveBuilt) m_drive.postStep(dt); }
    // PERFORMANCE PARTS -> the CANON car. The perf shop (app/perfshop.cpp) only
    // ever reached the --world drive DriveDemo, so every installed part, the ECU
    // tune and the whole knock model had ZERO effect on the car actually driven
    // in the game world. This forwards a composed tuning onto the live rig, the
    // same call the shop makes. Safe before the rig exists (no-op).
    // NOTE the LAZY BUILD: the live rig is not created by build() -- it is created
    // on the first enterCar() (world_cars.cpp, m_driveBuilt = true). A caller that
    // tunes right after build() therefore finds m_driveBuilt == false and the call
    // would silently do nothing (this is exactly what happened when the canon-car
    // tuning was first wired: the boot log printed the catalog line but the tuning
    // never reached the car). So CACHE the tuning and re-apply it the moment the
    // rig exists; enterCar() replays it.
    bool applyTuning(const x3::phys::WheeledTuning& t) {
        m_pendingTuning = t;
        m_haveTuning    = true;
        return m_driveBuilt ? m_drive.applyTuning(t) : false;
    }
    // True once the tuning has actually reached the live rig (not merely cached).
    bool tuningApplied() const { return m_driveBuilt && m_haveTuning; }
    // True once the live rig exists (so the host knows when applyTuning will stick).
    bool driveBuilt() const { return m_driveBuilt; }
    // Chase camera (the drive host's framing: 10 m back, 3.5 m up, orbits the
    // player's look angles).
    void driverCamera(float yaw, float pitch, float& x, float& y, float& z) const;
    void chassisVelocity(float out[3]) const;
    x3::phys::Vec3 carPosition() const;             // live car (driving) or origin
    float forwardSpeed() const { return m_driveBuilt ? m_drive.forwardSpeed() : 0.0f; }
    float engineRPM() const    { return m_driveBuilt ? m_drive.engineRPM() : 0.0f; }
    // PERF SHOP (NFS layer): the live rig for tuning/nitrous — PerfShop takes
    // a DriveDemo* and tolerates null (no car spawned yet / not driving).
    DriveDemo* liveCar() { return m_driveBuilt ? &m_drive : nullptr; }

    // Deep-water read on the LIVE car (engine-kill condition): water present
    // AND >= ~1.3 m deep over the local ground AND the hull is actually in it.
    bool inDeepWater() const;
    // The water-kill / scripted exit — same path as the E exit.
    void forceExit(Player* player, x3::phys::IPhysicsWorld& physics);

    // Engine audio while driving (RPM-pitched loop; the drive host treatment,
    // minimal). Safe with a null audio system.
    void updateAudio(x3::audio::IAudioSystem* audio, float throttle);

    // Draw the parked visuals + the live car. Call inside beginFrame/endFrame,
    // gated by the host on the outdoor PVS (kStreamedExteriorRoom visibility).
    void draw(const x3::rhi::FrameContext& frame) const;

    // Idempotent. Removes bodies + the live rig BEFORE physics dies.
    void shutdown(x3::phys::IPhysicsWorld& physics);

    // ---- Queries (tests / HUD) ---------------------------------------------
    uint32_t carCount() const { return (uint32_t)m_cars.size(); }
    const WorldCarDef& def(uint32_t i) const { return m_cars[i].def; }
    bool resident(uint32_t i) const { return m_cars[i].resident; }
    bool unlocked(std::string_view id) const {
        return m_unlocked.count(std::string(id)) != 0;
    }
    bool carLocked(uint32_t i) const {
        return m_cars[i].def.locked && !unlocked(m_cars[i].def.id);
    }
    x3::phys::Vec3 parkedPos(uint32_t i) const {
        return { m_cars[i].px, m_cars[i].py, m_cars[i].pz };
    }
    float hackProgress() const { return m_hackT / kCarHackSeconds; }
    // Nearest resident parked car within kCarReach of (x,z); -1 if none.
    int nearestCar(float x, float z) const;

private:
    struct Car {
        WorldCarDef def;
        float px = 0, py = 0, pz = 0;   // current parked pose (authored or re-parked)
        float yaw = 0;                  // radians, nose = +Z rotated by yaw
        bool  resident = false;         // parked visual + body present
        x3::phys::BodyId body{};        // static collision box (valid while resident)
    };

    void parkCar(Car& c, float x, float y, float z, float yaw,
                 x3::phys::IPhysicsWorld& physics);
    void unparkCar(Car& c, x3::phys::IPhysicsWorld& physics);
    bool enterCar(int idx, Player* player, x3::phys::IPhysicsWorld& physics);
    void exitCar(Player* player, x3::phys::IPhysicsWorld& physics);
    void drawParked(const x3::rhi::FrameContext& frame, const Car& c) const;

    bool m_built = false;
    double m_bootMs = 0.0;
    std::vector<Car> m_cars;
    std::unordered_set<std::string> m_unlocked;  // hacked-open ids (host-lifetime)
    GroundFn m_ground;
    WaterFn  m_water;
    AlarmFn  m_alarm;
    std::string m_prompt;
    float m_hackT = 0.0f;
    int   m_hackIdx = -1;

    // The single live rig (built on first entry) + its limbo parking slab.
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;   // set at build (velocity reads)
    std::string m_glbDir;
    DriveDemo m_drive;
    bool m_driveBuilt = false;
    // Performance-parts tuning cached at boot, replayed when the lazy rig builds.
    x3::phys::WheeledTuning m_pendingTuning{};
    bool m_haveTuning = false;
    bool m_driving = false;
    int  m_drivenIdx = -1;
    x3::phys::BodyId m_limboSlab{};
    static constexpr float kLimboX = 4000.0f, kLimboY = -600.0f, kLimboZ = 4000.0f;

    // Parked visual: the hero-car GLB (all drawables, authored pose, origin on
    // the ground) or the graybox fallback.
    std::unique_ptr<x3::asset::IAssetSource> m_glbSrc;
    std::unique_ptr<x3::asset::IModelLoader> m_glbLoader;
    x3::asset::Model m_glbModel;
    std::vector<x3::asset::ModelDrawable> m_glbDraw;
    bool m_skinned = false;
    x3::rhi::MeshHandle    m_boxMesh;     // graybox fallback chassis
    x3::rhi::MeshHandle    m_wheelMesh;   // graybox fallback wheels
    x3::rhi::TextureHandle m_whiteTex;

    // Alarm chirp (lazy-loaded on first hack) + the driving engine loop.
    x3::audio::SoundHandle m_sndBuzz{}, m_sndChime{}, m_sndEngine{};
    x3::audio::LoopHandle  m_engineLoop{};
    bool m_sndLoaded = false;
};

// Headless self-test (--test-canonvehicle). Flat-slab physics world + a real
// Player capsule + two authored cars (one unlocked "apron" car, one LOCKED
// region-owned car). Asserts: V1 E within reach enters (player capsule
// stashed, driving on); V2 4 s of throttle displaces the car > 10 m and it
// stays ON the ground; V3 exit restores the capsule GROUNDED beside the car;
// V4 a locked car refuses entry, hold-E for 3 s unlocks it + fires the alarm
// hook exactly once (releasing early resets progress); V5 the unlocked latch
// SURVIVES a region teardown/rebuild cycle (fresh parked pose, still
// unlocked); V6 entering the hacked car now works. Deterministic, no window/
// Vulkan. Logs PASS/FAIL V#, returns true iff all pass.
bool runCanonVehicleSelfTest();

} // namespace x3::game
