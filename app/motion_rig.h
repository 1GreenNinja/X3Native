#pragma once
// ===========================================================================
// motion_rig.h — THE MOTION-DOMAIN VERIFICATION RIG.
//
// This is an INSTRUMENT, not a motion-blur fixture. Every temporal effect this
// engine grows -- motion blur, temporal upscaling, volumetric reprojection,
// SSGI reprojection, anything whose whole existence is a function of time --
// should be gated with it.
//
// WHY IT EXISTS
// -------------
// Every verification gate in this tree is a STILL-FRAME A/B from a STATIC
// camera: the capture-review law (docs/plans/PUNCHLIST_GTA6_MATCH.md:77-79),
// the dB receipts in PLAN_GTA6_CAMPAIGN.md, the --screenshot-showroom basins.
// Motion blur on a static camera with a static scene is MATHEMATICALLY THE
// IDENTITY FUNCTION. It scores 0 dB and reads as "no change" -- indistinguishable
// from "not implemented". The instrument literally could not fail on the
// absence, which is why the same finding was made correctly three separate
// times (RENDER_GAP_ROADMAP.md:35, VEHICLE_UPGRADES.md:130, RACING_WORLD.md:642)
// and never reached a lane. See docs/design/WHY_THE_SURVEYS_MISSED_IT.md.
//
// THE TWO QUESTIONS, ANSWERED SEPARATELY
// --------------------------------------
// A metric for this domain has to distinguish "the scene moved" from "the image
// is blurred by that movement". Those are two questions and they get two numbers:
//
//   temporalRms  -- RMS luminance difference between CONSECUTIVE frames.
//                   Answers "did anything move at all?". It is a PRECONDITION,
//                   never evidence of blur: it also drops when the camera simply
//                   moves less, which is exactly the confound that makes a naive
//                   frame-difference metric useless here.
//
//   anisotropy   -- RMS luminance GRADIENT measured ALONG the known motion axis
//                   divided by the RMS gradient ACROSS it. Energy, not absolute
//                   value: total variation is CONSERVED when a step edge is
//                   blurred (an edge of height H spread over N pixels still sums
//                   to H), so a mean-|gradient| version would be nearly blind to
//                   the very thing it is measuring. Gradient energy falls as
//                   H^2/N and collapses once the blur exceeds the feature size.
//                   THIS is the discriminator, and the
//                   reason is a property of the two operations:
//                     * TRANSLATION IS AN ISOMETRY. Sliding an image sideways
//                       leaves its gradient statistics EXACTLY unchanged, so
//                       scene motion on its own cannot move this number.
//                     * MOTION BLUR IS A DIRECTIONAL CONVOLUTION. It attenuates
//                       detail along the motion axis and leaves detail across it
//                       almost untouched, so it MUST move this number, downward.
//                   A metric that fires on motion alone would be a guard that
//                   does not guard; this one cannot, by construction.
//
//   DBI          -- Directional Blur Index, 1 - anisotropy(test)/anisotropy(ref),
//                   between two series captured on the SAME deterministic path.
//                   0 = the two images are equally sharp along the motion axis.
//                   Positive = the test series lost detail specifically along
//                   the axis of motion, which is what motion blur IS.
//
// THE NEGATIVE CONTROL IS THE POINT
// ---------------------------------
// This tree has three "guards that don't guard" on record: a grounding test with
// no callers, a --test-factory flag claimed twice so the gate reported green
// while the test never ran, and a size guard that early-outed on exactly the case
// it existed to catch. So the rig must PROVE it can see the effect AND prove it
// does not hallucinate one:
//   * static camera, blur on vs off  -> DBI must be ~0 (and it is EXACTLY 0 by
//     construction: mb_blur.frag early-outs when the neighbourhood velocity is
//     under half a pixel, so the pass is the identity function);
//   * moving camera, blur on vs off  -> DBI must be measurably positive;
//   * blur off vs blur off           -> DBI must be exactly 0 (determinism);
//   * a synthetic directional blur   -> DBI must be positive (the metric works
//     on pixels alone, with no renderer involved).
// ===========================================================================
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::motionrig {

// ---------------------------------------------------------------------------
// PART 1 — the capture: a deterministic camera on rails.
// ---------------------------------------------------------------------------
// Fixed timestep, an analytic camera path (no spline file, no keyframes, nothing
// to drift), and an N-frame numbered PNG series. The scene is supplied by the
// caller as a draw callback taking the PATH TIME, so the same rig drives a
// synthetic probe scene, a real level, or anything else.
struct RailsConfig {
    int   frames = 8;          // captured frames
    // Frames rendered BEFORE capture starts, walking the same path backwards from
    // t0, so the camera is already in steady motion and TAA/auto-exposure have
    // converged when frame 0 is taken.
    //
    // (settle + frames) SHOULD BE A MULTIPLE OF 8. The TAA jitter phase is a
    // free-running frame counter with an 8-frame Halton cycle; aligning the
    // series length to it means two successive series start on the same phase,
    // which is what makes the determinism check bit-exact instead of approximate.
    int   settle = 16;
    float dt     = 1.0f / 60.0f;   // FIXED. Never a wall clock.
    float t0     = 0.0f;           // path time of captured frame 0

    // Camera: a lateral dolly along +X at camSpeed m/s, looking down +Z.
    // camSpeed 0 IS THE NEGATIVE CONTROL -- a genuinely static camera.
    float camSpeed = 0.0f;
    float camX = 0.0f, camY = 0.0f, camZ = -9.0f;
    float yaw = 1.57079633f, pitch = 0.0f, fov = 65.0f;

    std::string dir;                 // output directory (created if absent)
    std::string prefix = "frame";    // <dir>/<prefix>_000.png ...
};

// Draw one frame of the caller's scene at path time `t`. Called between
// beginFrame() and endFrame(); `t` is the ONLY time source the scene may use, so
// the series is reproducible.
using RailsDrawFn = void (*)(void* ctx, x3::rhi::IRenderDevice& device,
                            x3::rhi::FrameContext& frame, float t);

// Renders settle+frames frames and writes `frames` numbered PNGs. Returns the
// written paths in order; empty on failure.
std::vector<std::string> captureRails(x3::rhi::IRenderDevice& device,
                                      const RailsConfig& cfg,
                                      RailsDrawFn draw, void* drawCtx);

// ---------------------------------------------------------------------------
// PART 2 — the measurement.
// ---------------------------------------------------------------------------

// Region of interest as fractions of the frame, [0,1]. Default = whole frame.
struct Region {
    float x0 = 0.0f, y0 = 0.0f, x1 = 1.0f, y1 = 1.0f;
};

// Which screen axis the motion is along. The rig's camera path is a lateral
// dolly, so its motion axis is Horizontal; an object moving vertically or a
// pitching camera would use Vertical.
enum class MotionAxis { Horizontal, Vertical };

// A decoded RGBA8 frame. Exposed so the metric can be run on synthetic pixels
// (the metric's own self-check) without a renderer or a file on disk.
struct RgbaImage {
    int w = 0, h = 0;
    std::vector<uint8_t> px;      // w*h*4
    bool valid() const { return w > 0 && h > 0 && px.size() == (size_t)w * h * 4; }
};
RgbaImage loadImage(const std::string& path);

struct SeriesMetrics {
    int    frames      = 0;
    double gradAlong   = 0.0;  // RMS dLuma along the motion axis
    double gradAcross  = 0.0;  // RMS dLuma across it
    double anisotropy  = 0.0;  // gradAlong / gradAcross
    double temporalRms = 0.0;  // RMS consecutive-frame luma difference
    bool   valid       = false;
};

// Measure a numbered frame series. Frames must be the same size.
SeriesMetrics measureSeries(const std::vector<std::string>& pngs,
                            MotionAxis axis, Region roi = {});
// Same, on already-decoded frames (the metric's own self-check path).
SeriesMetrics measureFrames(const std::vector<RgbaImage>& frames,
                            MotionAxis axis, Region roi = {});

// Directional Blur Index of `test` against `ref`. Both must come from the SAME
// rails config, so any difference is the effect under test and nothing else.
// Returns 0 when either anisotropy is unusable.
double directionalBlurIndex(const SeriesMetrics& test, const SeriesMetrics& ref);

// ---------------------------------------------------------------------------
// PART 3 — small shared helpers the gates need.
// ---------------------------------------------------------------------------

struct ImageDelta {
    double meanAbs = 0.0;   // mean |channel delta|, 0..255
    int    maxAbs  = 0;     // max |channel delta|
    double psnrDb  = 99.0;  // 99 == bit-identical
    bool   valid   = false;
};
ImageDelta compareImages(const std::string& a, const std::string& b);

// True iff every byte of every listed pair matches. Used for the byte-identity
// claims (velocity-absent fallback, series determinism).
bool seriesBitIdentical(const std::vector<std::string>& a,
                        const std::vector<std::string>& b);

// METRIC SELF-CHECK, no renderer involved: convolve `src` with a box filter of
// `radiusPx` along `axis`. measureFrames must score the result as directionally
// blurred relative to the original, and must NOT score the original that way.
// This proves the METRIC works independently of whether the SHADER works -- so a
// broken shader and a broken probe cannot cancel out and read green.
RgbaImage applyDirectionalBlur(const RgbaImage& src, MotionAxis axis, int radiusPx);

} // namespace x3::motionrig
