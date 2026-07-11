#pragma once
// Salvari crystal-matrix WORMHOLE VFX (Act-3 system-jump transition).
//
// The Act-3 signature transition is a "crystal-matrix translation": the player's
// ship jumps between star systems through a crystalline energy tunnel. This is the
// standalone, reusable VFX prototype (built like the procedural starfield was) so a
// later subsystem (S3 space-engine autopilot/transit) can place it + drive its
// `progress` from the jump sequence.
//
// Aesthetic: a CRYSTALLINE / FACETED energy tunnel the camera flies down --
// blue -> purple -> white-hot, prismatic glints, energy streaking along the
// facets toward/past the camera, and a bright convergence point ahead. NOT a
// generic warp-stripe tunnel: the geometry is genuinely faceted (flat-shaded
// crystal panes) and the texture carries crystalline facet/streak structure.
//
// Approach (mirrors app/sky_stars.* -- read that header for the rationale):
// IRenderDevice intentionally HIDES custom-pipeline creation behind
// drawMesh/drawMeshEmissive/drawMeshGlass, and engine/rhi/* is OFF-LIMITS this
// lane. So instead of a raw custom fragment pipeline, the wormhole is an
// INSIDE-OUT, FLAT-FACETED tube mesh carrying a baked crystal-matrix texture,
// drawn through the existing drawMeshEmissive() HDR path. The ENERGY FLOW is
// produced by (a) the camera advancing down the tunnel axis over time in the
// showcase, and (b) a per-frame emissive PULSE whose phase scrolls with timeSec,
// so light reads as racing along the facets. `progress` (0..1) drives the
// white-hot core / convergence intensity as the jump completes.
//
// Clean-room: no idTech / Doom / Quake source consulted; all math is standard
// procedural-tube generation + value-noise facet shading.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::space {

class WormholeVfx {
public:
    // Per-frame tunable knobs. Defaults are the recommended Salvari look.
    struct Tuning {
        float length      = 200.0f;  // tunnel length (world units, along +Z by convention)
        float radius      = 14.0f;   // tunnel radius
        float flowSpeed   = 8.0f;    // energy streak speed toward the camera
        float facetDensity= 24.0f;   // crystalline facet count around the ring
        float coreColor[3]   = {0.9f, 0.95f, 1.3f};  // white-hot core (HDR)
        float wallColor[3]   = {0.35f, 0.5f, 1.25f}; // blue
        float accentColor[3] = {0.7f, 0.35f, 1.2f};  // purple glints
    };

    // Build the faceted tunnel mesh + bake the crystal-matrix texture through the
    // device. The tunnel runs along +Z from z=0 to z=length, centered on the
    // origin in X/Y. Idempotent: a second init() without a shutdown() is a no-op.
    void init(rhi::IRenderDevice& dev, const Tuning& t = {});

    // Render the wormhole for THIS frame. Emits one drawMeshEmissive() against the
    // tunnel mesh. `viewProj16` is accepted for API parity / future shader-path use
    // (the baked-texture path does not need it -- the camera transform is owned by
    // the host via setCamera()). `timeSec` animates the energy flow; `progress`
    // (0..1) intensifies the white-hot core + convergence as the jump completes.
    // Tuning is consumed every frame so a caller can scrub live.
    void render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                const float* viewProj16, float timeSec, float progress,
                const Tuning& t = {});

    // Place the tunnel mouth in the world. The model transform translates the mesh
    // so its z=0 mouth sits at (ox,oy,oz); the tunnel extends +length along +Z.
    // S3 sets this so the tunnel surrounds the ship's flight path. If never called
    // the tunnel sits at the origin (fine for the showcase).
    void setOrigin(float ox, float oy, float oz);

    // Destroy the mesh + texture. Idempotent.
    void shutdown(rhi::IRenderDevice& dev);

    // ---- Introspection (used by --test-wormhole) --------------------------
    bool initialized() const { return m_initialized; }
    rhi::MeshHandle    mesh() const    { return m_mesh; }
    rhi::TextureHandle texture() const { return m_tex; }
    const Tuning&      lastTuning() const { return m_lastTuning; }
    float              lastProgress() const { return m_lastProgress; }
    // Last per-object emissive strength the render path applied (the core pulse).
    float              lastCoreStrength() const { return m_lastCore; }

    // CPU reference of the baked crystal-matrix brightness for a point on the
    // tunnel wall, parameterized by angle `theta` (0..2pi around the ring) and
    // axial `zNorm` (0..1 along the length). Returns a brightness in [0,~2] (HDR).
    // The test uses this to assert the bake is non-trivial + faceted.
    static float sampleFacetBrightness(float theta, float zNorm, const Tuning& t);

private:
    rhi::MeshHandle    m_mesh{};
    rhi::TextureHandle m_tex{};
    bool               m_initialized = false;
    Tuning             m_lastTuning{};
    float              m_lastProgress = 0.0f;
    float              m_lastCore     = 0.0f;
    float              m_ox = 0.0f, m_oy = 0.0f, m_oz = 0.0f;
};

// Clamp the public Tuning fields to SAFE ranges:
//   length      > 0
//   radius      > 0
//   flowSpeed   >= 0
//   facetDensity>= 3 (need at least a triangle around the ring)
// Colors are left untouched (HDR positive multipliers are allowed). progress is
// clamped separately in render() to [0,1].
WormholeVfx::Tuning clampTuning(const WormholeVfx::Tuning& t);

// --test-wormhole: headless self-test of the WormholeVfx lifecycle + baked
// crystal-matrix (integration-feast fold; body lives in wormhole_vfx.cpp).
bool runWormholeSelfTest();

} // namespace x3::space
