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

// ---------------------------------------------------------------------------
// THE MULTI-SHELL INTERIOR (movie-grade pass, feat/wormhole-transit-ride)
// ---------------------------------------------------------------------------
// A single tube carrying one baked noise octave, scrolled uniformly, is the
// game-grade tell: every part of the frame moves at the same rate, so the eye
// reads one flat sheet sliding by and the speed never lands. Film VFX hold up
// because detail exists at SEVERAL frequencies and each frequency moves at its
// OWN rate — broad shape slow, mid filament faster, fine grain fastest — which
// is what produces parallax and what makes a tunnel read as a volume.
//
// The RHI still exposes no custom pipelines (drawMesh*/drawMeshGlass only), so
// the frequencies are built out of GEOMETRY rather than out of a shader: THREE
// CONCENTRIC SHELLS at different radii, each with its own bake at its own
// feature scale, its own roll rate and its own scroll offset. Because they sit
// at different distances from an on-axis camera, the inner shells sweep past
// faster in screen space for free — real parallax, not a faked one.
//
//   Shell 0 WALL      : outermost, OPAQUE (drawMeshEmissive). Broad filament
//                       structure, slow. The backdrop and the depth cue.
//   Shell 1 FILAMENT  : mid, ADDITIVE GLASS. Twisting mid-scale filament that
//                       counter-rolls against the wall.
//   Shell 2 GRAIN     : innermost, ADDITIVE GLASS. Fine sparks/grain, fastest
//                       roll and scroll — the layer that sells velocity.
//
// The inner two go through drawMeshGlass in ADDITIVE mode with the shell's own
// texture bound, so their contribution is emissive * TEXEL: black texels stay
// black and the layers COMPOSITE instead of overwriting. Nothing here applies a
// uniform per-object emissive lift to an opaque draw — that is the failure this
// effect has lost iterations to three times (see the note in render()).
constexpr int kWormholeShells = 3;

class WormholeVfx {
public:
    // Per-frame tunable knobs. Defaults are the recommended Salvari look.
    struct Tuning {
        float length      = 200.0f;  // tunnel length (world units, along +Z by convention)
        float radius      = 14.0f;   // tunnel radius (SHELL 0; inner shells scale down)
        float flowSpeed   = 8.0f;    // energy streak speed toward the camera
        float facetDensity= 24.0f;   // crystalline facet count around the ring
        float coreColor[3]   = {0.9f, 0.95f, 1.3f};  // white-hot core (HDR)
        float wallColor[3]   = {0.35f, 0.5f, 1.25f}; // blue
        float accentColor[3] = {0.7f, 0.35f, 1.2f};  // purple glints
        // ---- MULTI-FREQUENCY BAKE CONTROLS (the movie-grade pass) ----------
        // `detail` multiplies the filament feature frequency of the bake: shell 0
        // bakes at 1.0 (broad), shell 1 at ~2.7, shell 2 at ~6.5. `grain` is the
        // weight of the highest octave (the spark/grit band) — near zero on the
        // wall, dominant on the innermost shell. Defaults reproduce the original
        // single-shell look for every existing caller.
        float detail = 1.0f;
        float grain  = 0.15f;
        // Peak ceiling for the bake, BEFORE the HDR multiplier. Held below 1.0
        // through a soft (Reinhard) rolloff rather than a hard clamp, so the
        // brightest region still has gradient inside it instead of clipping to a
        // flat white patch with no detail — the film-vs-game tell.
        float peak = 0.94f;
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

    // Spin the tunnel about its OWN axis (+Z), radians. The tube is centred on
    // X/Y, so this is a pure roll of the throat around whatever is inside it —
    // the "tunnel banks and rolls around the ship" read. Applied before the
    // origin translation. Persistent, like setOrigin.
    void setRoll(float rad);

    // ---- THE RIDE DRAW (one shell copy) -----------------------------------
    // Draw ONE concentric shell at ONE world origin, rolled about its own axis,
    // at an HDR `gain`. The transit choreographer (wormhole_transit.cpp) calls
    // this several times per frame — several shells x several axial copies — to
    // assemble an endless tunnel around the camera. Splitting it this way keeps
    // the LOOK here and the CHOREOGRAPHY there.
    //
    // `shell` is clamped into [0, kWormholeShells). Shell 0 is opaque; 1..N are
    // additive glass, so draw order between them does not matter. `gain` scales
    // the shell's HDR drive (the caller fades copies in/out at the far end so
    // nothing pops into existence). No-op before init().
    void renderShell(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                     int shell, const float origin[3], float rollRad,
                     float gain, float timeSec, float progress);

    // Geometry the choreographer needs to tile the tunnel.
    float shellLength() const { return m_lastTuning.length; }
    float shellRadius(int shell) const;
    static int shellCount() { return kWormholeShells; }

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
    // Shell 0's handles are ALSO exposed as mesh()/texture() so every existing
    // caller and --test-wormhole keep working unchanged.
    rhi::MeshHandle    m_mesh{};
    rhi::TextureHandle m_tex{};
    rhi::MeshHandle    m_shellMesh[kWormholeShells]{};
    rhi::TextureHandle m_shellTex[kWormholeShells]{};
    float              m_shellRadius[kWormholeShells] = { 0.0f, 0.0f, 0.0f };
    bool               m_initialized = false;
    Tuning             m_lastTuning{};
    float              m_lastProgress = 0.0f;
    float              m_lastCore     = 0.0f;
    float              m_ox = 0.0f, m_oy = 0.0f, m_oz = 0.0f;
    float              m_roll = 0.0f;
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
