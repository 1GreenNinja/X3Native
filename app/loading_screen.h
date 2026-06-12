#pragma once
// EFLZ LOADING SCREEN (Task #49).
//
// A self-contained, dependency-light loading overlay shown on cold boot AND on
// every world switch (--world canonlevel / level1 / ...), replacing the bare /
// black gap between window creation and the first gameplay frame.
//
// It is built ONLY from the public IRenderDevice 2D draw path (drawHudQuad +
// drawHudTextF — the exact same path app/ui.cpp uses) plus a single procedurally-
// generated full-bleed background texture. No new pipeline, no extra deps.
//
// Pieces:
//   * a full-bleed themed background (sci-fi prison/lab vibe): a dark vertical
//     gradient with a faint scanline + vignette, generated procedurally at init
//     so we never block on art. Drawn as ONE textured quad.
//   * the title "ESCAPE FROM LAB ZERO" (Orbitron / FontRole::Title).
//   * a REAL progress bar (0->1) driven by the actual boot/world-build steps —
//     the host calls step()/setProgress() as each chunk of work completes.
//   * a rotating tip/lore line under the bar that cycles every ~2 s.
//   * a clean fade-in (on first frame) + fade-out (handed off to gameplay) so the
//     first rendered scene frame doesn't pop.
//
// HEADLESS SAFETY: init() / render() are no-ops without a valid device frame, so
// the headless --smoketest path (no window, FrameContext invalid) never blocks
// and never deadlocks. The progress + tip + fade STATE all advance regardless of
// whether a frame is drawn, so the logic is identical headed vs. headless.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// The ordered, WEIGHTED boot/world-build steps. Each entry advances the bar to a
// known cumulative fraction so progress is real (driven by work completing), not a
// timer. kDone == 1.0. The host pushes these as it finishes each chunk of work.
enum class LoadStep : uint32_t {
    Start          = 0,   // 0.00 — window + device just up
    DeviceReady    = 1,   // 0.10 — render device initialized
    AssetsMounted  = 2,   // 0.20 — asset paks/dirs mounted
    AudioReady     = 3,   // 0.30 — audio system + sounds loaded
    PhysicsReady   = 4,   // 0.40 — physics world up
    WorldGeometry  = 5,   // 0.65 — level/canon geometry + textures built
    Spawns         = 6,   // 0.85 — enemies / rescue / props / lights spawned
    FxReady        = 7,   // 0.95 — combat FX / debris / UI primed
    Done           = 8,   // 1.00 — first gameplay frame ready
    Count          = 9,
};

class LoadingScreen {
public:
    LoadingScreen();

    // Create the procedural background texture on `device`. Safe to call with a
    // headless device (createTexture returns a handle; nothing is drawn until a
    // valid frame). Idempotent: a second call is a no-op.
    void init(x3::rhi::IRenderDevice& device);
    // Release the background texture (optional; the device frees on shutdown).
    void shutdown(x3::rhi::IRenderDevice& device);

    // ---- Progress (real, driven by load steps) -----------------------------
    // Jump the bar to the cumulative fraction of `step`. Monotonic: never goes
    // backwards (a later call with an earlier step is clamped to the high-water
    // mark). `label` is an optional short status word shown on the bar.
    void step(LoadStep step, const char* label = nullptr);
    // Set the bar directly to `frac` (clamped 0..1, monotonic). Used for the
    // smooth sub-step interpolation between coarse steps if a caller wants it.
    void setProgress(float frac, const char* label = nullptr);
    float progress() const { return m_progress; }

    // ---- Per-frame --------------------------------------------------------
    // Advance the tip-cycle clock + fade by `dt` (seconds) and draw the overlay
    // into the given frame. A no-op draw when `frame.valid` is false (headless),
    // but the clock/fade STILL advance so the logic is deterministic everywhere.
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame, float dt);

    // Begin the fade-OUT (hand-off to gameplay). After this the overlay fades from
    // full to transparent over kFadeOut seconds; faded() returns true once gone.
    // `fast` (BOOT-TIME, docs/BOOT_TIME.md): a sub-budget boot barely showed the
    // bar — fade ~3x quicker so the hand-off doesn't pad the boot.
    void beginFadeOut(bool fast = false);
    bool fadingOut() const { return m_fadeOut; }
    bool faded() const { return m_fadeOut && m_fade <= 0.0f; }

    // Current alpha [0..1] the overlay is drawn at (fade-in ramps up, fade-out down).
    float alpha() const { return m_fade; }

    // Which tip index is currently shown (for tests). Cycles 0..tipCount-1.
    int tipIndex() const { return m_tipIndex; }
    int tipCount() const;
    const char* tip(int i) const;

private:
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame);

    x3::rhi::TextureHandle m_bg{};        // procedural full-bleed background
    bool   m_inited   = false;

    float  m_progress = 0.0f;             // high-water-mark fraction [0..1]
    std::string m_label;                  // short status word on the bar

    // Tip rotation.
    float  m_tipTimer = 0.0f;
    int    m_tipIndex = 0;

    // Fade state: m_fade ramps 0->1 (fade-in) then 1->0 (fade-out).
    float  m_fade     = 0.0f;
    bool   m_fadeOut  = false;
    float  m_fadeOutScale = 1.0f;   // 3.0 on a fast (sub-budget) boot hand-off
};

// --test-loading: headless self-test. Asserts progress is monotonic 0->1 across
// the load steps and that the tip line rotates (cycles through the array over
// time). Runs with NO window / Vulkan. Returns true on all-pass.
bool runLoadingSelfTest();

} // namespace x3::game
