#pragma once
// ===========================================================================
// motion_blur_test.h — --test-motionblur
//
// The gate for BOTH deltas of the motion lane: the motion-domain verification
// rig (app/motion_rig.h) and the motion-blur resolve pass (shaders/mb_*.frag).
//
// It runs on the REAL Vulkan device, over a purpose-built probe scene, and every
// claim it makes is measured from pixels the engine actually produced.
//
// Cases, in the order they run:
//
//  R1  RIG DETERMINISM — the same rails config twice is BIT-IDENTICAL.
//      Guards the instrument before anything is measured with it. Without this
//      every number below is noise.
//  R2  METRIC SELF-CHECK (positive) — a synthetic horizontal box blur applied to
//      a real captured frame IN MEMORY must score DBI > 0. Proves the METRIC can
//      see directional blur with no renderer involved, so a broken shader and a
//      broken probe cannot cancel out and read green.
//  R3  METRIC SELF-CHECK (negative) — the same frames against themselves must
//      score DBI exactly 0. Proves the metric does not manufacture a reading.
//  R4  PROBE SANITY — the reference series must actually contain measurable
//      detail (a black or featureless frame would let everything below pass
//      vacuously).
//
//  N1  NEGATIVE CONTROL, STATIC CAMERA — camera still, scene still, blur ON vs
//      OFF must be BIT-IDENTICAL and score DBI ~ 0. THIS IS THE POINT OF THE
//      LANE: motion blur on a static camera is the identity function, and an
//      instrument that reports it as blurred is measuring something else.
//  N2  NOTHING MOVED — the static series' frame-to-frame energy is ~0, so N1 is
//      a statement about a genuinely still frame and not about a still-looking
//      one.
//
//  P1  MOVING CAMERA, THE SCENE MOVED — frame-to-frame energy is large in BOTH
//      the ON and OFF series, so P2 compares two series that really are in motion.
//  P2  MOVING CAMERA, THE IMAGE IS BLURRED ALONG THE MOTION AXIS — DBI is
//      measurably positive. Blur ON vs OFF separates.
//
//  D1  dt NORMALISATION, ARITHMETIC — motionBlurVelocityScale() times a
//      proportionally smaller per-frame velocity gives the SAME exposure
//      displacement at 60 and 165 Hz.
//  D2  dt NORMALISATION, NEGATIVE CONTROL — the UN-normalised quantity differs by
//      the framerate ratio, so D1/D3 would go red if the rule were removed.
//  D3  dt NORMALISATION, RENDERED — the same physical camera path stepped at
//      1/60 and at 1/165, sampled at the SAME wall-clock instant, produces the
//      same measured blur strength. Compared as DBI-vs-own-reference rather than
//      as raw images, because TAA's own convergence is per-frame and therefore
//      legitimately framerate-dependent; this isolates the blur.
//
//  Z1  DEPTH ORDERING — a static foreground pillar in front of a fast-moving
//      background slab, static camera. The slab's pixels must blur; the pillar's
//      must NOT. This is the artefact that makes cheap motion blur look cheap.
//  Z2  DEPTH ORDERING, POSITIVE HALF — the moving slab in the same frames must
//      score clearly blurred, so Z1 is a statement about ordering and not about
//      a frame where nothing blurred at all.
//
//  V1  VELOCITY ABSENT — with r_velocity off, blur ON is BIT-IDENTICAL to blur
//      OFF. The graceful-degradation contract, measured rather than asserted.
// ===========================================================================
#include "engine/rhi/IRenderDevice.h"

#include <string>

namespace x3::game {

// Returns a process exit code: 0 = every case passed.
int runMotionBlurTest(x3::rhi::IRenderDevice& device, const std::string& outDir);

} // namespace x3::game
