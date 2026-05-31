#pragma once
// DECLOAK shimmer VFX (intro cinematic: a capital ship phasing into existence).
//
// For the game intro a Salvari/enemy capital ship hides in Jupiter's shadow and
// DECLOAKS -- shimmers from near-invisible to fully visible -- before opening
// fire on the player. This is the standalone, reusable VFX prototype (built like
// app/space/wormhole_vfx.* and app/sky_stars.* were) so the intro cinematic
// subsystem can place it at the capital ship's transform + drive `progress`
// 0->1 over the decloak beat.
//
// Aesthetic: at low progress a faint, animated CYAN-WHITE energy shimmer /
// scanline / edge-glow traces the ship silhouette (a "phase shell" hugging the
// hull bounds). As progress rises the shimmer INTENSIFIES, distorts, then
// RESOLVES; `revealAlpha(progress)` ramps 0->1 so the HOST fades the real ship
// mesh in underneath. The VFX class draws ONLY the shimmer/edge-glow overlay
// that sells the reveal -- the host draws the actual ship mesh modulated by
// revealAlpha().
//
// Approach (mirrors app/space/wormhole_vfx.h -- read that header for the
// rationale): IRenderDevice intentionally HIDES custom-pipeline creation behind
// drawMesh/drawMeshEmissive/drawMeshGlass, and engine/rhi/* is OFF-LIMITS this
// lane. So instead of a raw custom fragment pipeline, the decloak is a SHELL
// MESH (a scaled-up box hugging the ship bounds) carrying a baked, animated
// CYAN-WHITE shimmer/scanline texture with a per-object EMISSIVE glow, drawn
// through the existing translucent drawMeshGlass() HDR path so it COMPOSITES OVER
// the ship (which the host draws opaque underneath) instead of occluding it. The
// shimmer SCROLL is produced by a
// per-frame emissive-pulse phase that advances with timeSec; `progress` (0..1)
// drives the shimmer INTENSITY then resolve, and a window curve so the overlay
// fades out near progress 1 (the fully-revealed solid ship needs no shimmer).
//
// Clean-room: no idTech / Doom / Quake source consulted; all math is standard
// procedural value-noise + scanline shimmer.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::space {

class DecloakVfx {
public:
    // Per-frame tunable knobs. Defaults are the recommended capital-ship look.
    struct Tuning {
        float shellScale  = 1.12f;  // shell box size vs the ship-local unit bound (>1 hugs+haloes the hull)
        float shimmerSpeed= 3.0f;   // scanline/shimmer scroll rate
        float scanDensity = 18.0f;  // horizontal scanline count over the shell
        float edgeColor[3]   = { 0.55f, 0.95f, 1.35f };  // cyan-white energy edge glow (HDR)
        float coreColor[3]   = { 0.9f,  1.0f,  1.25f };  // white-hot shimmer peaks (HDR)
    };

    // Build the shell-box mesh + bake the cyan-white shimmer texture through the
    // device. The shell is a unit-ish box centered on the origin (the ship-local
    // model transform scales/places it over the actual ship bounds). Idempotent:
    // a second init() without a shutdown() in between is a no-op.
    void init(rhi::IRenderDevice& dev, const Tuning& t = {});

    // Render the decloak shimmer overlay for THIS frame at `modelTransform16`
    // (column-major 4x4, the ship's world placement -- the shell is drawn in the
    // ship's local frame so it hugs the hull). `viewProj16` is accepted for API
    // parity / future shader-path use (the baked-texture path does not need it).
    // `timeSec` scrolls the shimmer; `progress` (0..1): 0 = fully cloaked (faint
    // shimmer outline), ~0.5 = peak shimmering/distorting reveal, 1 = resolved
    // (overlay fades out, host draws the solid ship). Tuning is consumed every
    // frame so a caller can scrub live.
    void render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                const float* viewProj16, const float* modelTransform16,
                float timeSec, float progress, const Tuning& t = {});

    // How opaque the SHIP MESH ITSELF should be drawn at this progress (the host
    // multiplies its ship-draw tint/alpha by this). A smoothstep 0->1 ramp:
    // revealAlpha(0) ~= 0 (ship invisible / fully cloaked), revealAlpha(1) ~= 1
    // (ship fully solid), monotonic non-decreasing in between. Pure + static so
    // the test + the host can call it without an instance.
    static float revealAlpha(float progress);

    // Destroy the mesh + texture. Idempotent.
    void shutdown(rhi::IRenderDevice& dev);

    // ---- Introspection (used by --test-decloak) ---------------------------
    bool initialized() const { return m_initialized; }
    rhi::MeshHandle    mesh() const    { return m_mesh; }
    rhi::TextureHandle texture() const { return m_tex; }
    const Tuning&      lastTuning() const { return m_lastTuning; }
    float              lastProgress() const { return m_lastProgress; }
    // Last per-object shimmer strength the render path applied (peaks mid-reveal,
    // fades out near progress 1). The test asserts this is mid-peaked.
    float              lastShimmerStrength() const { return m_lastShimmer; }

    // CPU reference of the baked shimmer brightness at shell UV (u,v in [0,1)).
    // Returns a brightness in [0,~2] (HDR). The test uses this to assert the bake
    // is non-trivial (scanline + edge variance).
    static float sampleShimmerBrightness(float u, float v, const Tuning& t);

private:
    rhi::MeshHandle    m_mesh{};
    rhi::TextureHandle m_tex{};
    bool               m_initialized = false;
    Tuning             m_lastTuning{};
    float              m_lastProgress = 0.0f;
    float              m_lastShimmer  = 0.0f;
};

// Clamp the public Tuning fields to SAFE ranges:
//   shellScale  > 0
//   shimmerSpeed>= 0
//   scanDensity >= 1
// Colors are left untouched (HDR positive multipliers are allowed). progress is
// clamped separately in render() / revealAlpha() to [0,1].
DecloakVfx::Tuning clampTuning(const DecloakVfx::Tuning& t);

} // namespace x3::space
