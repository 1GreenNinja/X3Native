#pragma once
// Vehicle demo worlds (drive / boat / fly) — game/slice layer over the engine's
// GENERAL vehicle framework (engine/physics/IVehicle.h).
//
// Game/slice code only; engine/ stays pure. Three self-contained demo scenes,
// each = a dynamic rigid body + one IVehicleController + render meshes:
//   * DriveDemo : a wheeled car (Jolt VehicleConstraint) on the STREAMED terrain.
//   * BoatDemo  : a buoyant hull floating on a flat ocean (sea level), + a sub
//                 mode that can dive.
//   * FlyDemo   : an aircraft (thrust + lift + drag + attitude control).
//
// Built ONLY through the public IRenderDevice + IPhysicsWorld + IVehicleController
// interfaces. No id Tech / RBDOOM source. Low-conflict with the rest of the app
// (own world, its own --world flags).

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/IVehicle.h"
#include "engine/asset/IModelLoader.h"   // hero-car GLB skin (ModelDrawable)
#include "engine/asset/IAssetSource.h"
#include "mesh_prims.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace x3::game {

// A simple Y-aligned cylinder mesh (radius 1, height 1: spans y in [-0.5,0.5]).
// Used for the car wheels; the vehicle controller's wheel transform already bakes
// the radius/width scale, so the renderer just draws this unit cylinder.
void makeUnitCylinderY(uint32_t segments,
                       std::vector<x3::rhi::MeshVertex>& verts,
                       std::vector<uint32_t>& idx);

// ---------------------------------------------------------------------------
// DriveDemo — a wheeled car on flat ground OR on streamed terrain. The car is a
// chassis box (dynamic, layer Dynamic) + a WheeledVehicleController with 4 raycast
// wheels. The host steps physics; this owns the controller + render meshes and
// draws the chassis (full rotation) + the 4 wheels at their live transforms.
// ---------------------------------------------------------------------------
class DriveDemo {
public:
    // Spawn the car at (x,y,z). The chassis is a ~1300 kg box sized to the HERO
    // CAR GLB (CTR: 1.81 x 1.3 x 4.3 m, wheel stations from the model); 4 wheels
    // (front steered, rear powered + handbrake). Returns false if the controller
    // failed. build() = buildPhysics() + the graybox render meshes.
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z);

    // Physics-only build (no render device) — the headless self-test path.
    bool buildPhysics(x3::phys::IPhysicsWorld& physics, float x, float y, float z);

    // HERO-CAR SKIN: load the converted vehicle GLB (clearcoat paint + emissive
    // lights) and render IT instead of the graybox box+cylinders. The GLB's
    // Wheel_FL/FR/RL/RR node drawables follow the live physics wheel poses (spin +
    // steer + suspension); everything else rides the sprung chassis. The GLB is
    // authored nose = +Z / origin on the ground; the skin bakes the 180-degree yaw
    // to the engine's -Z forward + the ride-height drop. Returns false (graybox
    // render kept) if the GLB is missing — drive still works.
    bool skin(x3::rhi::IRenderDevice& device, std::string_view glbDir,
              std::string_view relPath);
    bool skinned() const { return m_skinned; }

    // All four wheels touching ground? (drive self-test assert)
    bool allWheelsInContact() const;

    // Feed driver input + advance one step. Call setInput()+preStep() BEFORE the
    // host's physics->step(), then postStep() AFTER. drive() is a convenience that
    // does setInput()+preStep() (host steps next).
    void setInput(const x3::phys::VehicleInput& in);
    void preStep(float dt);
    void postStep(float dt);

    // Draw the chassis + wheels. Call between beginFrame/endFrame.
    void render(const x3::rhi::FrameContext& frame) const;

    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId chassis() const { return m_chassis; }
    // World position of the chassis (for chasing the camera).
    void chassisPos(float out[3]) const;
    float forwardSpeed() const { return m_ctl ? m_ctl->forwardSpeed() : 0.0f; }
    float engineRPM()    const { return m_ctl ? m_ctl->engineRPM() : 0.0f; }

    // ---- Performance-shop live tuning passthrough (engine WheeledTuning). ----
    // Re-tunes the RUNNING Jolt vehicle in place (engine curve/torque, mass, grip,
    // suspension, ride height, brakes) — drive out of the shop and FEEL the parts.
    bool applyTuning(const x3::phys::WheeledTuning& t) {
        return m_ctl ? m_ctl->applyWheeledTuning(t) : false;
    }
    // Nitrous: temporary torque multiplier (1 = off). Host owns the tank timer.
    void  setTorqueBoost(float mult) { if (m_ctl) m_ctl->setTorqueBoost(mult); }
    float torqueBoost() const { return m_ctl ? m_ctl->torqueBoost() : 1.0f; }
    // Traction control (default ON — see setInput). Off = full burnout mode.
    void  setTractionControl(bool on) { m_tcEnabled = on; }
    bool  tractionControl() const { return m_tcEnabled; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_chassis;
    // Chassis half extents — sized to the hero-car GLB footprint (CTR).
    float m_hx = 0.84f, m_hy = 0.5f, m_hz = 1.95f;

    x3::phys::VehicleInput m_lastIn;     // raw driver input (pre-TC; HUD/audio)
    bool m_tcEnabled = true;             // traction control (see setInput)

    x3::rhi::MeshHandle    m_chassisMesh;
    x3::rhi::MeshHandle    m_wheelMesh;
    x3::rhi::TextureHandle m_chassisTex;
    x3::rhi::TextureHandle m_wheelTex;
    std::vector<x3::phys::WheelDesc> m_wheels;

    // ---- Hero-car GLB skin (optional; graybox fallback when absent) ----
    bool m_skinned = false;
    std::unique_ptr<x3::asset::IAssetSource> m_skinSrc;
    std::unique_ptr<x3::asset::IModelLoader> m_skinLoader;
    x3::asset::Model m_skinModel;
    std::vector<x3::asset::ModelDrawable> m_bodyDraw;     // everything but the wheels
    std::vector<x3::asset::ModelDrawable> m_wheelDraw[4]; // per physics wheel slot
    void drawDrawable(const x3::rhi::FrameContext& f,
                      const x3::asset::ModelDrawable& d, const float world[16]) const;
};

// Headless game-layer DRIVE self-test (--test-vehicle): spawn the car on a flat
// static slab, ENTER it (player control handed to the car), throttle N fixed
// ticks, assert FORWARD DISPLACEMENT + all-wheel ground contact, then EXIT and
// assert player control is restored (on-foot position placed beside the car).
// Physics-only (no render device); returns true when every check passes.
bool runDriveEnterExitSelfTest();

// ---------------------------------------------------------------------------
// BoatDemo — a buoyant hull floating on a flat ocean at `seaLevel`. `isSub` makes
// it a submarine (can dive with the dive input). The hull is a box; buoyancy +
// drag keep it at the waterline. Renders the hull at its full transform.
// ---------------------------------------------------------------------------
class BoatDemo {
public:
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z, float seaLevel, bool isSub = false);

    void setInput(const x3::phys::VehicleInput& in);
    void preStep(float dt);
    void postStep(float dt);

    void render(const x3::rhi::FrameContext& frame) const;
    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId hull() const { return m_hull; }
    void hullPos(float out[3]) const;
    float submergedFraction() const { return m_ctl ? m_ctl->submergedFraction() : 0.0f; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_hull;
    float m_hx = 1.5f, m_hy = 0.6f, m_hz = 3.0f;

    x3::rhi::MeshHandle    m_hullMesh;
    x3::rhi::TextureHandle m_hullTex;
};

// ---------------------------------------------------------------------------
// FlyDemo — an aircraft body driven by the flight controller (thrust + lift +
// drag + pitch/yaw/roll). Renders the airframe at its full transform.
// ---------------------------------------------------------------------------
class FlyDemo {
public:
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z);

    void setInput(const x3::phys::VehicleInput& in);
    void preStep(float dt);
    void postStep(float dt);

    void render(const x3::rhi::FrameContext& frame) const;
    void shutdown();

    x3::phys::IVehicleController* controller() const { return m_ctl.get(); }
    x3::phys::BodyId airframe() const { return m_body; }
    void airframePos(float out[3]) const;
    float forwardSpeed() const { return m_ctl ? m_ctl->forwardSpeed() : 0.0f; }

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_body;
    float m_hx = 2.0f, m_hy = 0.4f, m_hz = 2.6f;

    x3::rhi::MeshHandle    m_bodyMesh;
    x3::rhi::MeshHandle    m_wingMesh;
    x3::rhi::TextureHandle m_bodyTex;
};

} // namespace x3::game
