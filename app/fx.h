#pragma once
// Combat FX: shot tracers + muzzle flash (gameplay-feel pass).
//
// Game/slice code only — engine/ stays pure. Built from the IRenderDevice +
// Vec3 interfaces only. No id Tech / RBDOOM source consulted.
//
// WHY world-space geometry: tracers + muzzle flash are real world-space boxes
// (the only thing the mesh path can draw is a mesh at a column-major transform):
//   * Tracer: a thin bright box stretched from the muzzle to the hit point (or to
//     max range on a miss), shown for a fraction of a second.
//   * Muzzle flash: a brief bright box at the muzzle.
//
// The crosshair USED to live here as a world-space "+"; as of S7 it moved to the
// new screen-space HUD layer (app/hud.*), which draws a crisp, depth-free 2D
// reticle at the framebuffer center via IRenderDevice::drawHudQuad. CombatFx no
// longer draws a crosshair.

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>

namespace x3::game {

// ---- Tracer tuning ----
// How long (seconds) a shot tracer beam stays visible. Long enough to actually
// READ as a streak leaving the barrel (0.06 was ~3 frames -> invisible).
constexpr float kTracerTime      = 0.12f;
// Half-thickness (m) of the tracer beam box (cross-section is 2*thickness). A
// hair-thin beam viewed nearly end-on (it leaves the gun toward the crosshair)
// is invisible; fatten it so the shot clearly comes out of the weapon.
constexpr float kTracerThickness = 0.035f;
// Max concurrent tracers (pool). Excess shots overwrite the oldest slot.
constexpr int   kMaxTracers      = 8;

// ---- Muzzle flash tuning ----
// How long (seconds) the muzzle flash quad stays visible.
constexpr float kMuzzleFlashTime = 0.04f;
// Half-extent (m) of the muzzle flash box.
constexpr float kMuzzleFlashSize = 0.05f;

// ---- GPU-instanced particle pool (combat juice) ----
// Bounded CPU-simulated, GPU-instanced billboard pool. The CPU integrates each
// live particle (pos/vel/gravity/drag/life/fade) into a FIXED ring (no per-frame
// heap alloc) and submits the live ones to the device as camera-facing billboards
// each frame (additive for sparks/fire/muzzle, alpha for smoke/dust/blood). The
// device draws them in the HDR pass before bloom (bright sparks glow) with a soft
// depth fade against the scene depth. Spawned ONLY from combat events, so the pool
// is empty (zero GPU cost) until a weapon fires / something is hit / dies.
constexpr int kMaxParticles = 4096;   // pool capacity (bounded; oldest recycled)

// ---- Impact decal ring ----
// Bounded ring of impact decals (bullet holes / scorch marks). Each is an oriented
// quad laid on the hit surface at the raycast hit point + normal; the oldest is
// recycled when full. Persistent until recycled or faded out over its lifetime.
constexpr int   kMaxDecals    = 64;     // ring capacity (oldest recycled)
constexpr float kDecalLife    = 12.0f;  // seconds before a decal fully fades

// Combat FX system. Owns a couple of shared unit box meshes (created in init,
// destroyed in shutdown) and a small pool of active tracers / a muzzle flash
// timer. Self-contained: no Scene/physics state.
class CombatFx {
public:
    // Create the shared meshes (a unit box). Call once after the device is up.
    void init(x3::rhi::IRenderDevice& device);

    // Register a shot beam from `from` (the muzzle) to `to` (the hit point, or
    // eye + dir*range on a miss). Also lights the muzzle flash at `from` AND spawns
    // the muzzle-flash particle burst (a few hot additive sparks at the muzzle).
    void addTracer(const x3::phys::Vec3& from, const x3::phys::Vec3& to);

    // ---- Combat-event particle/decal presets (the juice) -------------------
    // Each spawns a tuned burst into the bounded pool / decal ring. Called from the
    // existing combat hooks (weapon fire, melee, monster hit/death). `dir` is the
    // shot/impact direction (need not be unit); `normal` is the surface normal.

    // Muzzle flash burst at the muzzle, biased along the fire `dir` (additive).
    void spawnMuzzleFlash(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir);
    // Bullet hit on a hard surface: a cone of additive sparks + an alpha dust puff,
    // sprayed back along the surface `normal`. Also drops a scorch DECAL at the hit.
    void spawnImpact(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal);
    // Hit on an enemy: a short spray of dark-red alpha blood along the shot `dir`.
    void spawnBlood(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir);
    // Enemy death: a burst of debris chunks (alpha, gravity) + a lingering smoke
    // puff so the kill reads on screen.
    void spawnDeath(const x3::phys::Vec3& pos);
    // Lingering smoke puff (alpha, slow rise) — used by death + as a generic cue.
    void spawnSmoke(const x3::phys::Vec3& pos);
    // Drop a scorch decal directly (bullet-hole / impact mark) at a hit point+normal.
    void addDecal(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal);

    // Advance the FX timers (tracer lifetimes, muzzle flash) AND simulate the
    // particle pool (integrate pos/vel/gravity/drag/life) + age the decals. No-op
    // at dt <= 0 for the timer decay; the sim is integrated each call.
    void update(float dt);

    // Draw the crosshair (always, at screen center in front of the camera) and
    // any active tracers + muzzle flash. `eye` + camera `yaw`/`pitch` build the
    // camera basis for the crosshair. Call AFTER scene/viewmodel/monster draws.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              float eyeX, float eyeY, float eyeZ, float yaw, float pitch) const;

    // Submit the live particles (camera-facing billboards) + decals to the device
    // for THIS frame. Call between beginFrame/endFrame, after draw(). No-op when
    // the pool + decal ring are empty (zero GPU cost when idle).
    void submit(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Live particle count (for --bench reporting / debug).
    int liveParticleCount() const;

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

    // ---- CPU particle pool (bounded; GPU-instanced billboards) -------------
    // One slot per particle. `life <= 0` means a free slot. Each frame update()
    // integrates the live ones and submit() streams them to the device. The spawn
    // helpers find a free slot (or recycle the oldest) — no per-frame heap alloc.
    struct Particle {
        x3::phys::Vec3 pos{};
        x3::phys::Vec3 vel{};
        float life     = 0.0f;     // remaining seconds (<=0 == free)
        float maxLife  = 1.0f;     // for the fade curve
        float size0    = 0.1f;     // half-extent at birth (m)
        float size1    = 0.1f;     // half-extent at death (m)
        float r = 1, g = 1, b = 1; // linear RGB (scaled by intensity at birth)
        float a0       = 1.0f;     // opacity at birth
        float gravity  = 0.0f;     // * world gravity (m/s^2 along -Y)
        float drag     = 0.0f;     // per-second velocity damping (0 = none)
        bool  additive = true;     // additive (sparks) vs alpha (smoke/blood)
    };
    Particle m_particles[kMaxParticles];
    int      m_nextParticle = 0;   // round-robin recycle cursor

    // Spawn one particle into a free/recycled slot (returns the slot index).
    int  spawnParticle(const Particle& p);
    // Small fast deterministic PRNG (xorshift) for spawn jitter — no <random>
    // churn, repeatable in headless captures.
    uint32_t m_rng = 0x1234567u;
    float frand();                 // [0,1)
    float frandSym();              // [-1,1)

    // ---- Impact decal ring (oriented quads on hit surfaces) ----------------
    struct Decal {
        x3::phys::Vec3 center{};
        x3::phys::Vec3 normal{ 0, 1, 0 };
        float halfSize = 0.1f;
        float angle    = 0.0f;     // spin about the normal (rad)
        float life     = 0.0f;     // remaining seconds (<=0 == free)
        float maxLife  = kDecalLife;
    };
    Decal m_decalsRing[kMaxDecals];
    int   m_nextDecal = 0;         // round-robin recycle cursor

    // Per-frame submit scratch (member-owned so submit() does NO heap/stack alloc
    // of the big instance arrays; mutable so the const submit() can fill them).
    mutable x3::rhi::IRenderDevice::ParticleInstance m_scratchAdd[kMaxParticles];
    mutable x3::rhi::IRenderDevice::ParticleInstance m_scratchAlpha[kMaxParticles];
    mutable x3::rhi::IRenderDevice::DecalInstance     m_scratchDecal[kMaxDecals];
};

} // namespace x3::game
