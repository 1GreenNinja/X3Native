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
#include <string_view>

namespace x3::game {

// Per-weapon FX look. The WeaponDef carries string FX-id hints (muzzleFx/impactFx);
// the host maps those onto one of these via fxKindFromId() so each gun's muzzle
// flash + impact read distinctly (color/size) instead of one shared generic burst.
//   Default  : the original hot orange-white ballistic look (pistol-grade).
//   Pistol   : the same ballistic look (an explicit alias so the roster is readable).
//   Smg      : leaner/cooler ballistic — fast small sparks (auto bloom).
//   Shotgun  : a WIDE, fat muzzle flash + a broad spark spray (heavy boom).
//   Chaingun : hot + extra-sparky (a busy, glowing auto roar).
//   Plasma   : BLUE energy — cool tint, soft round flash, no metal sparks.
//   Lightning: electric CRACKLE — white-cyan, twitchy fast sparks (beam zap).
enum class WeaponFxKind : uint8_t {
    Default = 0,
    Pistol,
    Smg,
    Shotgun,
    Chaingun,
    Plasma,
    Lightning,
};

// Map a WeaponDef FX-id string (e.g. "muzzle_plasma", "impact_bullet") onto a
// WeaponFxKind. Recognizes the substrings the roster uses; anything unknown ->
// Default. Pure + header-inline so both the host and tests can call it.
inline WeaponFxKind fxKindFromId(std::string_view id) {
    auto has = [&](std::string_view s) { return id.find(s) != std::string_view::npos; };
    if (has("plasma"))    return WeaponFxKind::Plasma;
    if (has("lightning")) return WeaponFxKind::Lightning;
    if (has("chaingun"))  return WeaponFxKind::Chaingun;
    if (has("shotgun"))   return WeaponFxKind::Shotgun;
    if (has("smg"))       return WeaponFxKind::Smg;
    if (has("pistol"))    return WeaponFxKind::Pistol;
    return WeaponFxKind::Default;
}

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
// Lightning bolt ARC PROPAGATION speed (m/s): the jagged bolt visibly extends from
// the muzzle toward the hit point at this rate rather than snapping full-length the
// instant the (hitscan) beam fires. Director note: the old instant/over-fast read
// was reduced ~39% — this is the tuned, slower travel (0.61x of the prior feel).
// Tuned so a ~28 m max-range beam still completes well within the tracer lifetime
// (kTracerTime): at 300 m/s a 28 m beam fully connects in ~0.093 s (< 0.12 s life),
// so the tip is still visibly travelling yet always reaches the hit point.
constexpr float kLightningBoltSpeed = 300.0f;

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
    // `kind` tints/shapes the beam: Lightning renders as a jagged white-cyan bolt
    // (re-randomized each frame); everything else is the straight hot-yellow tracer.
    void addTracer(const x3::phys::Vec3& from, const x3::phys::Vec3& to,
                   WeaponFxKind kind = WeaponFxKind::Default);

    // ---- Combat-event particle/decal presets (the juice) -------------------
    // Each spawns a tuned burst into the bounded pool / decal ring. Called from the
    // existing combat hooks (weapon fire, melee, monster hit/death). `dir` is the
    // shot/impact direction (need not be unit); `normal` is the surface normal.

    // Muzzle flash burst at the muzzle, biased along the fire `dir` (additive). The
    // default overload keeps the original hot-orange ballistic look; the `kind`
    // overload tints + scales the burst per weapon (plasma blue, shotgun wide,
    // lightning crackle, ...) so each gun reads distinctly. The default forwards to
    // kind=Default, so existing callers (melee/monster) are unchanged.
    void spawnMuzzleFlash(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir);
    void spawnMuzzleFlash(const x3::phys::Vec3& pos, const x3::phys::Vec3& dir, WeaponFxKind kind);
    // Bullet hit on a hard surface: a cone of additive sparks + an alpha dust puff,
    // sprayed back along the surface `normal`. Also drops a scorch DECAL at the hit.
    // The `kind` overload tints the sparks per weapon (plasma = blue energy splash,
    // lightning = white-cyan, etc.); the default keeps the original metal-spark look.
    void spawnImpact(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal);
    void spawnImpact(const x3::phys::Vec3& pos, const x3::phys::Vec3& normal, WeaponFxKind kind);
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
        float          age  = 0.0f;  // seconds since spawn (Lightning bolt propagation)
        WeaponFxKind   kind = WeaponFxKind::Default;  // Lightning -> jagged bolt
    };

    // Draw one bright box stretched/oriented along the segment a->b with the
    // given half-thickness (cross-section), tinted by `color`.
    void drawBeam(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  const x3::phys::Vec3& a, const x3::phys::Vec3& b,
                  float thickness, const float color[4]) const;

    // Draw a JAGGED lightning bolt a->b: subdivide into segments with random
    // perpendicular offsets (re-rolled per call so it crackles), each drawn via
    // drawBeam. The last vertex lands exactly on `b` (the hit point).
    void drawLightningBolt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
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
        // Decal tint (linear rgb). Default == dark scorch (bullet hole); blood pools
        // override it to dark red. Submit multiplies the lifetime fade into alpha.
        float color[3] = { 0.02f, 0.015f, 0.01f };
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
