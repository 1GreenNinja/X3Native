#pragma once
// MOTION BLUR — shared constants and the dt normalisation rule.
//
// This header exists so the shutter/dt arithmetic has ONE definition that both
// the renderer (which fills the pass UBO) and the test battery (which asserts
// framerate invariance) call.  A formula duplicated between a shader-parameter
// fill and its test is a guard that does not guard: the test would pass while the
// shipped path drifted.  Everything here is pure and header-only.
//
// THE RULE — "delta time, never frame time", in a new place.
// ---------------------------------------------------------
// shaders/velocity.frag writes (prevUV - curUV): a displacement accumulated over
// ONE FRAME.  It is not a speed.  At 165 Hz that displacement is ~1/5 of what it
// is at 33 Hz for identical physical motion.  Scaling blur by it directly makes
// the effect VANISH at high framerate and OVERWHELM at low framerate -- i.e.
// strongest exactly when the machine is struggling, which is backwards.
//
// What a real camera integrates is a fixed EXPOSURE DURATION in seconds.  So:
//
//     physical rate (UV/s) = velocityUV / dt
//     exposure seconds     = shutter / referenceFps
//     blurUV               = rate * exposure
//                          = velocityUV * shutter * (1/referenceFps) / dt
//                          = velocityUV * motionBlurVelocityScale(dt, ...)
//
// `shutter` is the shutter fraction AT THE REFERENCE FRAMERATE: 0.5 is the film
// 180-degree shutter, and at referenceFps = 60 that is an 8.33 ms exposure at
// every framerate.  Halving dt halves velocityUV and doubles the scale, so
// blurUV -- the thing the eye sees -- is invariant.  That invariance is asserted
// end to end by the motion-domain rig (--test-motionblur, case "dt-invariance").

#include <algorithm>

namespace x3 { namespace rhi {

// Tile edge in screen pixels for the tile-max reduction (stage 1).
// MUST equal `kTile` in shaders/mb_tilemax.frag, mb_neighbormax.frag, mb_blur.frag.
inline constexpr int kMotionBlurTile = 20;

// Neighbour-max search radius in TILES (stage 2).
// MUST equal `kReach` in shaders/mb_neighbormax.frag.
inline constexpr int kMotionBlurReach = 2;

// The exactness invariant of the gather dilation: a pixel is only ever told
// about fast tiles within kMotionBlurReach tiles of it, so a blur longer than
// this could come from a tile it never inspected and the sharp-silhouette
// artefact returns.  Every blur length is clamped to this bound.
inline constexpr float kMotionBlurMaxRadiusPx =
    float(kMotionBlurTile) * float(kMotionBlurReach);

// Defaults.  The effect ships OFF (r_motionblur 0) -- it changes every frame of
// every world, so it is opted into, not discovered.  When enabled, 0.5 shutter
// at a 60 Hz reference is the conventional film 180-degree look.
inline constexpr float kMotionBlurDefaultShutter = 0.5f;
inline constexpr float kMotionBlurDefaultRefFps  = 60.0f;
inline constexpr int   kMotionBlurDefaultSamples = 9;

// Sample-count bounds.  Odd counts are not required; the centre tap is added
// separately and weighted as 1/S regardless.
inline constexpr int kMotionBlurMinSamples = 3;
inline constexpr int kMotionBlurMaxSamples = 31;

// The multiplier applied to the per-frame UV velocity to obtain the exposure's
// UV displacement.  See the derivation above.
//
// dt <= 0 (first frame, a stalled clock, a headless caller that never measured)
// returns the reference-rate scale, i.e. treats this frame as if it lasted
// exactly one reference frame.  That is the only behaviour that cannot produce a
// spike: returning a huge scale on a bad dt would flash the screen to mush.
inline float motionBlurVelocityScale(float dtSeconds, float shutter, float refFps) {
    if (!(refFps > 0.0f)) refFps = kMotionBlurDefaultRefFps;
    if (shutter < 0.0f)   shutter = 0.0f;
    const float refFrameTime = 1.0f / refFps;
    if (!(dtSeconds > 0.0f)) return shutter;              // == shutter * refFrameTime/refFrameTime
    // Numeric floor ONLY (1 ms => 1000 fps).  Deliberately NOT a behavioural
    // clamp: capping the ratio would cap it at HIGH framerate, where dt is small
    // and the ratio is large -- exactly the case the normalisation exists to
    // serve.  The blur LENGTH is bounded downstream by motionBlurMaxRadius(),
    // which is the right place for that, and a long-frame hitch reduces the
    // ratio (correctly: a fixed exposure is a smaller slice of a longer frame).
    const float dt = (dtSeconds < 1.0e-3f) ? 1.0e-3f : dtSeconds;
    return shutter * (refFrameTime / dt);
}

// Clamp the r_mb_maxblur cvar to the bound the gather dilation can actually
// honour.  <= 0 means "use the bound".
inline float motionBlurMaxRadius(float requestedPx) {
    if (!(requestedPx > 0.0f)) return kMotionBlurMaxRadiusPx;
    return std::min(requestedPx, kMotionBlurMaxRadiusPx);
}

inline int motionBlurClampSamples(int s) {
    return std::clamp(s, kMotionBlurMinSamples, kMotionBlurMaxSamples);
}

}} // namespace x3::rhi
