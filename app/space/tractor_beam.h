#pragma once
// Tractor-beam VFX (game intro: the decloaked capital ship CAPTURES Jake's fighter).
//
// ART DIRECTION (owner, 2026-07-11): "Tractor beam is too cartoony. Think of how
// this looks in real TV shows. It's a faint, blue beam, shimmering and pulsing
// with power, but not a solid rendered object." Target: a Star-Trek-class tractor
// beam -- FAINT, TRANSLUCENT (the scene visible THROUGH it), alive with subtle
// motion. The previous look was a solid striped cyan cone (candy-cane); THIS is
// the retune to a faint translucent beam.
//
// HOW THE FAINT-TRANSLUCENT LOOK IS BUILT (engine tools, no engine/rhi edits):
//   * TRANSLUCENCY: both cones are drawn through drawMeshPBR(..., alphaBlend=true)
//     with the baseColorFactor alpha in the NEAR-CLEAR canopy-glass band. mesh.frag
//     honors an authored alpha in (0, 0.07) LITERALLY (outA = a + faint fresnel
//     edge) -- so the scene reads straight through the beam. The outer HAZE cone
//     sits at ~0.05, the inner CORE at ~0.10. BLEND draws are partitioned AFTER the
//     opaques by the device (the S6/glass ordering law) -- no host ordering needed.
//   * STRUCTURE: two NESTED cones -- a wide, very faint OUTER HAZE + a narrow,
//     slightly firmer INNER CORE. No candy stripes.
//   * TEXTURE: LOW-CONTRAST blue-white energy fields (never saturated cyan). The
//     core field carries a longitudinal falloff (brightest at the emitter,
//     feathering to near-nothing at the captured end); the haze field is a tileable
//     band pattern (the power "pulses") + a whisper of noise (shimmer).
//   * MOTION (the "alive" part):
//       (a) PULSE  -- the haze cone's UV V is PANNED along the beam each frame
//           (updateMesh, the ship_windows portal pattern; REPEAT sampler wraps it
//           seamlessly) so the bands flow emitter->capture (power flowing in).
//       (b) BREATHE -- the per-draw emissive strength is modulated +/-18% on a slow
//           ~0.7 Hz sine plus a tiny ~6 Hz +/-5% flicker (shimmer without strobe);
//           the core breathes slightly OUT OF PHASE with the haze.
//   * GLOW: brightness rides emissiveTex (texture-gated, the durable ACES recipe)
//     at MODEST strength over a near-black baseColor rgb, so the beam GLOWS faintly
//     rather than rendering as lit plastic (no flat emissive white-clip).
//   * EMITTER / IMPACT: a soft glow orb at the emitter mouth + a fainter shimmer orb
//     where the beam meets the captured ship sell the effect on TV.
//
// Approach (unchanged spine): IRenderDevice hides pipeline creation, so the beam is
// baked UNIT meshes (cones authored along +Z, apex/emitter at z=0, base/capture at
// z=1) re-oriented + stretched onto the live from->to axis each frame via a model
// matrix this class computes. Clean-room: no idTech / Doom / Quake source consulted.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <vector>

namespace x3::space {

class TractorBeam {
public:
    // Per-frame tunable knobs. Defaults are the recommended faint-TV look.
    struct Tuning {
        float emitterRadius = 0.22f;  // thin CORE cone radius at the EMITTER end
        float captureRadius = 1.1f;   // thin CORE cone radius at the CAPTURED-SHIP end
        float hazeScale     = 2.3f;   // wide HAZE cone radius = core radius * this (soft envelope)
        float coreAlpha     = 0.10f;  // inner-core blend alpha (translucent core)
        float hazeAlpha     = 0.05f;  // outer-haze blend alpha (near-clear canopy band)
        float ringDensity   = 2.5f;   // soft luminance bands along the beam (power pulses)
        float flowSpeed     = 0.55f;  // haze UV V-pan speed (units/sec, emitter->capture)
        float coreColor[3]  = { 0.55f, 0.75f, 1.0f };  // faint BLUE-WHITE (linear; never candy cyan)
        float edgeColor[3]  = { 0.28f, 0.48f, 0.85f };  // deeper blue wash at the cone edge
    };

    // Build the two cone meshes + the glow orb + bake the two energy textures.
    // Idempotent: a second init() without a shutdown() in between is a no-op.
    void init(rhi::IRenderDevice& dev, const Tuning& t = {});

    // Render the tractor beam for THIS frame: outer haze cone + inner core cone
    // (both alpha-blended translucent) + an emitter glow orb + a faint impact orb.
    // Apex/emitter at `from`, base/capture at `to`. `viewProj16` is accepted for API
    // parity (the baked path does not need it). `intensity` (0..1) is the lock-on
    // ramp (scales brightness). `timeSec` drives the flow pan + the breathe. A
    // degenerate from==to (near-zero length) is handled gracefully: the draw is
    // skipped (no NaN, no degenerate transform).
    void render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                const float* viewProj16, const float from[3], const float to[3],
                float intensity, float timeSec, const Tuning& t = {});

    // Destroy the meshes + textures. Idempotent.
    void shutdown(rhi::IRenderDevice& dev);

    // ---- Introspection (used by --test-tractor) ---------------------------
    bool initialized() const { return m_initialized; }
    rhi::MeshHandle    mesh() const     { return m_meshCore; }   // primary (core) mesh
    rhi::MeshHandle    hazeMesh() const { return m_meshHaze; }
    rhi::MeshHandle    orbMesh() const  { return m_meshOrb; }
    rhi::TextureHandle texture() const  { return m_texGrad; }    // primary (core) texture
    rhi::TextureHandle hazeTexture() const { return m_texBands; }
    const Tuning&      lastTuning() const { return m_lastTuning; }
    float              lastIntensity() const { return m_lastIntensity; }
    // Last per-object CORE beam strength the render path applied (0 when degenerate).
    float              lastStrength() const { return m_lastStrength; }
    // The blend alphas the two cones were drawn with on the last render (0 when
    // degenerate). Both are TRANSLUCENT (well under 1.0) -- the test asserts this.
    float              lastHazeAlpha() const { return m_lastHazeAlpha; }
    float              lastCoreAlpha() const { return m_lastCoreAlpha; }
    // True if the beam is drawn through the alpha-blend (translucent) path. Always
    // true once the art-direction retune landed; kept for an explicit gate assertion.
    bool               alphaBlended() const { return m_alphaBlended; }
    // True if the most recent render() actually emitted draws (false when the
    // from/to pair was degenerate and the beam was skipped).
    bool               lastDrawn() const { return m_lastDrawn; }

    // CPU reference of the baked CORE energy brightness at sAlong (0 emitter -> 1
    // capture). Returns a brightness in [0,~2]. The test uses this to assert the
    // core field is a longitudinal gradient (EMITTER end brighter -- the new look)
    // with band structure along the axis.
    static float sampleEnergyBrightness(float sAlong, const Tuning& t);

private:
    rhi::MeshHandle    m_meshCore{};
    rhi::MeshHandle    m_meshHaze{};
    rhi::MeshHandle    m_meshOrb{};
    rhi::TextureHandle m_texGrad{};   // longitudinal falloff field (the wide HAZE + orbs)
    rhi::TextureHandle m_texBands{};  // tileable band field (the thin CORE, scrolled)
    // Base (un-panned) CORE vertices; render() re-uploads a V-panned copy each frame
    // so the thin bright core's bands FLOW emitter->capture (power flowing in).
    std::vector<rhi::MeshVertex> m_coreBaseVerts;
    std::vector<rhi::MeshVertex> m_coreScratch;
    bool               m_initialized   = false;
    Tuning             m_lastTuning{};
    float              m_lastIntensity = 0.0f;
    float              m_lastStrength  = 0.0f;
    float              m_lastHazeAlpha = 0.0f;
    float              m_lastCoreAlpha = 0.0f;
    bool               m_alphaBlended  = true;
    bool               m_lastDrawn     = false;
};

// Clamp the public Tuning fields to SAFE ranges:
//   emitterRadius >= 0 ; captureRadius > 0 ; ringDensity >= 1 ; flowSpeed >= 0
//   hazeScale >= 1 ; coreAlpha/hazeAlpha clamped to (0, 0.95]
// Colors are left untouched (HDR positive multipliers are allowed). intensity is
// clamped separately in render() to [0,1].
TractorBeam::Tuning clampTuning(const TractorBeam::Tuning& t);

// --test-tractor: headless self-test of the TractorBeam lifecycle + baked
// energy fields + translucency (integration-feast fold; body lives in the .cpp).
bool runTractorSelfTest();

} // namespace x3::space
