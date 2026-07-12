#pragma once
// Procedural starfield system (Act-3 space backdrop + general deep-space backdrop).
//
// Draws an infinite, parallax-correct, twinkling starfield BEHIND all scene
// geometry. Self-contained, no asset dependency. Works in any world that wants
// stars (--world space, future Act-3 sector worlds, etc.).
//
// Approach: an inside-out skydome mesh ("celestial sphere") carries a procedural
// stable starfield baked into an equirectangular RGBA8 texture. Each pixel of
// the texture is the result of a view-direction hash (same math as the
// shaders/starfield.frag GLSL source-of-truth), so when the player rotates the
// view the stars stay anchored in world space and parallax correctly.
//
// The dome is drawn FAR (very large radius, no depth write) via the existing
// IRenderDevice::drawMeshEmissive path so this code adds NO RHI surface area
// (engine/rhi/* is untouched). For global twinkle, render() updates the
// per-frame emissive scalar -- per-star phases are baked into per-star color
// jitter so a single global sine still reads as a busy twinkling field.
//
// Clean-room: no idTech / Doom / Quake source consulted; all math derived from
// the shader pseudocode and standard procedural-noise / equirect projection.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3 {

class SkyStars {
public:
    // Per-frame tunable knobs. Defaults are the recommended deep-space look.
    // Density / radius / threshold drive how dense, how big, how bright the
    // starfield reads. Twinkle controls the global breathing rate (per-star
    // phase is hashed into the bake, so the global modulator still reads busy).
    struct Tuning {
        // NOTE: density/threshold tuned for the BAKED-TEXTURE path -- the
        // shader spec's 80 / 0.985 leaves only ~800 lit texels in a 1024x512
        // equirect, which the bilinear sampler smears to invisibility. The
        // higher density + lower threshold here yield ~5-10% lit texels, so
        // the dome reads as a busy starfield in the screenshot pixel-variance
        // gate (uniqColors >> 100). The shader-pipeline path (if/when the RHI
        // lane lands the starfield pipeline) can use the spec defaults
        // directly since per-pixel evaluation doesn't suffer the texture
        // resolution averaging.
        float starDensity   = 200.0f;    // direction-space cells per unit
        float starRadius    = 0.4f;      // pixel-relative star half-size
        float threshold     = 0.85f;     // hash threshold (0..1) -- bigger = sparser
        float twinkleSpeed  = 2.0f;      // Hz-ish global twinkle rate
        float baseColor[3]  = { 1.0f, 1.0f, 1.05f };   // slight blue-white tint
    };

    // Create the celestial-sphere mesh + bake the procedural starfield texture
    // through the device. The dome lives at the FAR plane; the texture is
    // baked once on init() using the SAME hash math as shaders/starfield.frag.
    // Safe to call once; calling twice WITHOUT a shutdown() in between is a
    // no-op (idempotent on already-initialized).
    void init(rhi::IRenderDevice& dev, const Tuning& t = {});

    // Render the starfield for this frame. Emits one drawMeshEmissive() call
    // against the celestial-sphere mesh; the global emissive scalar is modulated
    // by sin(time * twinkleSpeed). `viewProjInv16` and `timeSec` are accepted
    // for parity with the design API (shader-path would use them per-frame);
    // in the baked-texture path the dome is camera-anchored implicitly (the
    // mesh transform tracks the camera so the sphere never "moves away").
    //
    // Tuning t is consumed every frame so a caller can scrub density / color
    // live without rebuilding (the baked texture stays fixed; only twinkle +
    // base-color multiplier respond).
    void render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                const float* viewProjInv16, float timeSec,
                const Tuning& t = {});

    // Update the camera position so the sphere stays centered on the eye. Call
    // BEFORE render() each frame (the render path uses the cached camera pos
    // to compose the model transform). If never called, the sphere stays at
    // the origin -- which is fine for the screenshot showcase but not for a
    // free-flying player.
    void setCamera(float ex, float ey, float ez);

    // Destroy the mesh + texture. Idempotent (calling without an init() is a
    // no-op).
    void shutdown(rhi::IRenderDevice& dev);

    // ---- Introspection (used by --test-starfield) -------------------------
    bool initialized() const { return m_initialized; }
    rhi::MeshHandle     mesh() const   { return m_mesh; }
    rhi::TextureHandle  texture() const { return m_tex; }
    // Last clamped tuning passed to render(); the test asserts the clamp.
    const Tuning&       lastTuning() const { return m_lastTuning; }
    // Last emissive strength applied (for the twinkle modulator test).
    float               lastEmissiveStrength() const { return m_lastEmissive; }

    // CPU-side reference of the SAME procedural hash the GLSL frag uses. The
    // test calls this to assert the API parameter clamping path and that the
    // bake produces a non-trivial starfield. Returns the brightness in [0,1]
    // for the world-space direction `dir` under `t` at `timeSec` -- summed
    // across the two layers (bright + dust) so a t==0 sample matches the bake.
    static float sampleProceduralBrightness(const float dir[3],
                                            const Tuning& t,
                                            float timeSec);

private:
    rhi::MeshHandle    m_mesh{};
    rhi::TextureHandle m_tex{};
    bool               m_initialized = false;
    Tuning             m_lastTuning{};
    float              m_lastEmissive = 0.0f;
    float              m_camX = 0.0f, m_camY = 0.0f, m_camZ = 0.0f;
};

// Clamp the public Tuning fields to the SAFE-VALUE ranges documented above:
//   density   > 0
//   radius    > 0
//   threshold in [0, 1)
//   twinkle   >= 0
// Returns a copy with the same fields clamped (callable from tests). The base
// color is left untouched (HDR-ish positive multipliers are allowed).
SkyStars::Tuning clampTuning(const SkyStars::Tuning& t);

} // namespace x3
