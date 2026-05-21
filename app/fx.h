#pragma once
// Combat FX: crosshair + shot tracers + muzzle flash (gameplay-feel pass).
//
// Game/slice code only — engine/ stays pure. Built from the IRenderDevice +
// Vec3 interfaces only. No id Tech / RBDOOM source consulted.
//
// WHY world-space geometry: the slice has no 2D/UI/screen-space draw path — the
// only thing the device can draw is a mesh at a column-major model transform.
// So the crosshair and tracers are real world-space boxes:
//   * Crosshair: a tiny bright "+" placed a short fixed distance in front of the
//     camera and oriented to the camera basis, re-placed every frame so it tracks
//     the view and reads as a fixed screen-center reticle.
//   * Tracer: a thin bright box stretched from the muzzle to the hit point (or to
//     max range on a miss), shown for a fraction of a second.
//   * Muzzle flash: a brief bright box at the muzzle.
//
// CAVEAT (clip): because these are world-space and depth-tested, the crosshair
// (and a tracer that starts inside the near geometry) can be occluded by or clip
// into world geometry that is nearer than kCrosshairDist. There is no dedicated
// overlay/no-depth pass in the slice, so this is accepted; the distances/sizes
// are tuned small enough that it reads fine in the test level.

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>

namespace x3::game {

// ---- Crosshair tuning ----
// Distance (m) in front of the camera the crosshair "+" is placed.
constexpr float kCrosshairDist = 0.6f;
// Half-length (m) of each crosshair arm at kCrosshairDist (overall "+" extent).
constexpr float kCrosshairSize = 0.012f;

// ---- Tracer tuning ----
// How long (seconds) a shot tracer beam stays visible.
constexpr float kTracerTime      = 0.06f;
// Half-thickness (m) of the tracer beam box (cross-section is 2*thickness).
constexpr float kTracerThickness = 0.012f;
// Max concurrent tracers (pool). Excess shots overwrite the oldest slot.
constexpr int   kMaxTracers      = 8;

// ---- Muzzle flash tuning ----
// How long (seconds) the muzzle flash quad stays visible.
constexpr float kMuzzleFlashTime = 0.04f;
// Half-extent (m) of the muzzle flash box.
constexpr float kMuzzleFlashSize = 0.05f;

// Combat FX system. Owns a couple of shared unit box meshes (created in init,
// destroyed in shutdown) and a small pool of active tracers / a muzzle flash
// timer. Self-contained: no Scene/physics state.
class CombatFx {
public:
    // Create the shared meshes (a unit box). Call once after the device is up.
    void init(x3::rhi::IRenderDevice& device);

    // Register a shot beam from `from` (the muzzle) to `to` (the hit point, or
    // eye + dir*range on a miss). Also lights the muzzle flash at `from`.
    void addTracer(const x3::phys::Vec3& from, const x3::phys::Vec3& to);

    // Advance the FX timers (tracer lifetimes, muzzle flash). No-op at dt <= 0.
    void update(float dt);

    // Draw the crosshair (always, at screen center in front of the camera) and
    // any active tracers + muzzle flash. `eye` + camera `yaw`/`pitch` build the
    // camera basis for the crosshair. Call AFTER scene/viewmodel/monster draws.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              float eyeX, float eyeY, float eyeZ, float yaw, float pitch) const;

    // Destroy the shared meshes. Call once on exit (no VMA leaks).
    void shutdown(x3::rhi::IRenderDevice& device);

private:
    struct Tracer {
        x3::phys::Vec3 from{};
        x3::phys::Vec3 to{};
        float          life = 0.0f;  // remaining seconds; <= 0 means free slot
    };

    // Draw one bright box stretched/oriented along the segment a->b with the
    // given half-thickness (cross-section), tinted by `color`.
    void drawBeam(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                  float thickness, const float color[4]) const;

    // A centered unit box mesh (half-extent 0.5 each axis) reused for all FX,
    // scaled per draw via the model matrix.
    x3::rhi::MeshHandle m_box;

    Tracer m_tracers[kMaxTracers];
    int    m_nextTracer = 0;        // round-robin write cursor into the pool

    x3::phys::Vec3 m_muzzlePos{};
    float          m_muzzleFlash = 0.0f;  // remaining seconds
};

} // namespace x3::game
