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
#include "mesh_prims.h"

#include <cstdint>
#include <memory>
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
    // Spawn the car at (x,y,z). The chassis is a ~1500 kg box; 4 wheels (front
    // steered, rear powered + handbrake). `groundLayer` = the layer the wheels
    // raycast against (Static for terrain). Returns false if the controller failed.
    bool build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               float x, float y, float z);

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

private:
    x3::rhi::IRenderDevice*  m_device  = nullptr;
    x3::phys::IPhysicsWorld* m_physics = nullptr;
    std::unique_ptr<x3::phys::IVehicleController> m_ctl;
    x3::phys::BodyId m_chassis;
    float m_hx = 0.9f, m_hy = 0.4f, m_hz = 1.8f;   // chassis half extents

    x3::rhi::MeshHandle    m_chassisMesh;
    x3::rhi::MeshHandle    m_wheelMesh;
    x3::rhi::TextureHandle m_chassisTex;
    x3::rhi::TextureHandle m_wheelTex;
    std::vector<x3::phys::WheelDesc> m_wheels;
};

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
