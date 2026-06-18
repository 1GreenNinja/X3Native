#pragma once
// app/space/descent.h
//
// S4 — Cinematic atmospheric descent.
//
// The orbit->ground on-rails sequence that masks the load into a surface world.
// It registers a DESCENT RUNNER with the S0 SpaceLayer spine (the frozen
// `registerDescentRunner(TransitionFn)` contract): when the player triggers
// `SpaceLayer::requestDescent(planetId)`, S0 enters Context::AtmoDescent and
// ticks our runner each `update(dt)`. The runner ramps an internal timer over
// `durationSec`; while < 1.0 it returns false (descent still playing), and at
// 1.0 it returns true — at which point S0 lands in Context::Surface and hands
// off to the `--world` surface system.
//
// The visual side (render()) draws the atmospheric-entry effect WITHOUT adding
// any RHI surface area: it reuses the existing IRenderDevice emissive/glass draw
// paths over a small set of procedural meshes (the same self-contained pattern as
// app/sky_stars.*). Over the descent it composes:
//   - a fullscreen-ish HEAT/FOG tint (a camera-anchored inside-out dome) that
//     intensifies through mid-descent (the hull-glow / re-entry fireball) and
//     eases to a clear sky at the end, and
//   - several CLOUD-STREAK layers (camera-facing translucent slabs) that rush
//     past as the environment "falls" toward the ground.
//
// SURFACE HANDOFF (STUBBED — Wave 2): on completion the SpaceLayer reaches
// Context::Surface; the actual `--world` surface load is wired at integration
// time (S4 depends on the existing terrain/open-world `--world` system per the
// space-engine spec §4). This subsystem only OWNS the on-rails cinematic + the
// runner that flips S0 into Surface; it does not itself load a world.
//
// Clean-room: no idTech / Doom / Quake source consulted; all of this is derived
// from the engine's own RHI + the sky_stars dome pattern.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::space {

class SpaceLayer;  // forward decl — we only need a reference (frozen spine)

class AtmoDescent {
public:
    // Registers a SpaceLayer descent runner that runs an on-rails entry sequence
    // over `durationSec`, then returns true (SpaceLayer lands in Surface). Also
    // builds the (self-contained) GPU resources for the entry effect through the
    // device. `durationSec` is clamped to a small positive minimum so the runner
    // always advances. Idempotent: calling twice without shutdown() rebuilds the
    // runner registration but does not re-create GPU resources.
    void init(rhi::IRenderDevice& dev, SpaceLayer& layer, float durationSec = 8.0f);

    // Draw the atmospheric-entry effect for this frame: hull glow + atmosphere fog
    // + cloud streaks, modulated by the current descent `progress()`. `viewProj16`
    // is accepted for parity with the design API (the dome is camera-anchored in
    // this baked-mesh path); `timeSec` drives the cloud-streak scroll + flicker.
    // No-op if not initialized.
    void render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                const float* viewProj16, float timeSec);

    // 0..1 descent completion (the runner's ramp). 0 before the descent starts,
    // 1.0 once the on-rails sequence finishes.
    float progress() const { return m_progress; }

    // True while the descent runner is armed and ramping (progress in [0,1)).
    bool active() const { return m_active; }

    // Destroy GPU resources. Idempotent.
    void shutdown(rhi::IRenderDevice& dev);

    // ---- Introspection (used by --test-atmo-descent) ----------------------
    bool initialized() const { return m_initialized; }
    float durationSec() const { return m_duration; }

    // Set the camera position so the effect dome stays centered on the eye. Call
    // BEFORE render() each frame (mirrors SkyStars::setCamera). If never called
    // the dome stays at the origin (fine for the showcase capture).
    void setCamera(float ex, float ey, float ez);

private:
    // The registered runner advances this each tick; render() reads it.
    rhi::MeshHandle    m_dome{};        // inside-out fog/heat dome (camera-anchored)
    rhi::MeshHandle    m_streak{};      // a single quad reused for every cloud layer
    rhi::TextureHandle m_streakTex{};   // soft cloud-streak alpha texture
    bool   m_initialized = false;
    bool   m_active      = false;
    float  m_progress    = 0.0f;        // 0..1
    float  m_duration    = 8.0f;        // seconds
    float  m_timer       = 0.0f;        // elapsed seconds in the runner
    float  m_camX = 0.0f, m_camY = 0.0f, m_camZ = 0.0f;

    // Advance the runner by dt; returns true when the sequence completes (>=1.0).
    bool tick(float dt);
};

// Re-entry heat tint at descent progress `p` in [0,1]: a 0-at-start, peak-near-
// the-middle, ease-to-clear-at-the-end intensity curve (the hull-glow fireball).
// Exposed so --test-atmo-descent can assert the curve shape without a GPU.
float descentHeatIntensity(float p);

} // namespace x3::space
