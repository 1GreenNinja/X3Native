#pragma once
// Tractor-beam VFX (game intro: the decloaked capital ship CAPTURES Jake's fighter).
//
// The intro beat: a capital ship fires a TRACTOR BEAM -- a tapered energy cone --
// that locks onto Jake's fighter and PULLS it into the ship's hold. This is the
// standalone, reusable VFX prototype, built exactly like app/space/wormhole_vfx.*
// and app/sky_stars.* were: an emissive MESH wrapped in a baked animated energy
// texture, drawn through the existing IRenderDevice::drawMeshEmissive() HDR path.
//
// Aesthetic: a CONE that is WIDE at the captured-ship end and narrows toward the
// emitter (a tractor "funnel"), wrapped with concentric energy rings/pulses that
// SCROLL from the emitter toward the captured ship (the "pull" direction). The
// look is cyan/teal-white additive glow with EDGE FALLOFF so it reads as a soft
// energy beam, not a solid plastic tube. `intensity` (0..1) scales opacity/
// brightness -- the lock-on ramp as the beam grabs hold.
//
// Approach (mirrors wormhole_vfx.h -- read that header for the rationale):
// IRenderDevice intentionally HIDES custom-pipeline creation behind
// drawMesh/drawMeshEmissive/drawMeshGlass, and engine/rhi/* is OFF-LIMITS this
// lane. So the beam is a UNIT cone mesh (authored along +Z, apex at z=0, base at
// z=1) baked ONCE in init(); each render() re-ORIENTS + STRETCHES it onto the
// live from->to axis via a model matrix the class computes from the axis. The
// energy texture (radial rings + edge-falloff) is baked once; the per-frame PULSE
// + intensity are applied through the baseColor multiplier so the rings appear to
// flow toward the captured ship as it is reeled in.
//
// Clean-room: no idTech / Doom / Quake source consulted; all math is standard
// procedural cone generation + a baked radial-ring energy field.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::space {

class TractorBeam {
public:
    // Per-frame tunable knobs. Defaults are the recommended capital-ship look.
    struct Tuning {
        float emitterRadius = 0.6f;   // cone radius at the EMITTER end (narrow)
        float captureRadius = 3.0f;   // cone radius at the CAPTURED-SHIP end (wide funnel)
        float ringDensity   = 7.0f;   // concentric energy rings along the beam
        float flowSpeed     = 2.2f;   // ring scroll speed emitter->capture (the "pull")
        float coreColor[3]  = { 0.55f, 1.05f, 1.25f }; // cyan/teal-white core (HDR-ish, linear)
        float edgeColor[3]  = { 0.10f, 0.55f, 0.80f };  // deeper teal at the cone edge
    };

    // Build the unit cone mesh + bake the energy-ring texture through the device.
    // The cone is authored along +Z (apex/emitter at z=0, base/capture at z=1) and
    // re-oriented per frame in render(). Idempotent: a second init() without a
    // shutdown() in between is a no-op.
    void init(rhi::IRenderDevice& dev, const Tuning& t = {});

    // Render the tractor beam for THIS frame. Emits one drawMeshEmissive() against
    // the cone mesh, oriented so the apex sits at `from` (emitter) and the base at
    // `to` (captured ship). `viewProj16` is accepted for API parity / future
    // shader-path use (the baked-texture path does not need it). `intensity` (0..1)
    // scales opacity/brightness (the lock-on ramp). `timeSec` scrolls the energy
    // pulses from->to. Degenerate from==to (or near-zero length) is handled
    // gracefully: the draw is skipped (no NaN, no degenerate transform).
    void render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                const float* viewProj16, const float from[3], const float to[3],
                float intensity, float timeSec, const Tuning& t = {});

    // Destroy the mesh + texture. Idempotent.
    void shutdown(rhi::IRenderDevice& dev);

    // ---- Introspection (used by --test-tractor) ---------------------------
    bool initialized() const { return m_initialized; }
    rhi::MeshHandle    mesh() const    { return m_mesh; }
    rhi::TextureHandle texture() const { return m_tex; }
    const Tuning&      lastTuning() const { return m_lastTuning; }
    float              lastIntensity() const { return m_lastIntensity; }
    // Last per-object beam strength the render path applied (0 when degenerate).
    float              lastStrength() const { return m_lastStrength; }
    // True if the most recent render() actually emitted a draw (false when the
    // from/to pair was degenerate and the beam was skipped).
    bool               lastDrawn() const { return m_lastDrawn; }

    // CPU reference of the baked energy brightness at (theta around the cone,
    // sAlong 0..1 emitter->capture). Returns a brightness in [0,~2]. The test uses
    // this to assert the bake is non-trivial (rings + edge structure).
    static float sampleEnergyBrightness(float sAlong, const Tuning& t);

private:
    rhi::MeshHandle    m_mesh{};
    rhi::TextureHandle m_tex{};
    bool               m_initialized   = false;
    Tuning             m_lastTuning{};
    float              m_lastIntensity = 0.0f;
    float              m_lastStrength  = 0.0f;
    bool               m_lastDrawn     = false;
};

// Clamp the public Tuning fields to SAFE ranges:
//   emitterRadius >= 0
//   captureRadius  > 0
//   ringDensity   >= 1
//   flowSpeed     >= 0
// Colors are left untouched (HDR positive multipliers are allowed). intensity is
// clamped separately in render() to [0,1].
TractorBeam::Tuning clampTuning(const TractorBeam::Tuning& t);

} // namespace x3::space
