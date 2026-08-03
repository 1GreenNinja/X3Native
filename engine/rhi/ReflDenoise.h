// Reflection buffer DENOISE — the edge-aware a-trous filter, device-independent.
//
// CLEAN-ROOM, original work. Written from published, non-engine references only:
//   * Dammertz, Sewtz, Hanika, Lensch, "Edge-Avoiding A-Trous Wavelet Transform
//     for Fast Global Illumination Filtering" (HPG 2010) — the a-trous wavelet
//     with per-iteration tap DILATION and edge-stopping functions on auxiliary
//     geometry buffers.
//   * Schied et al., "Spatiotemporal Variance-Guided Filtering" (HPG 2017) — the
//     depth / normal edge-stopping weight formulation.
//   * Real-Time Rendering 4th ed. (bilateral / joint-bilateral filtering).
//   * This repository's OWN in-house precedent, which already solves the same
//     class of problem for screen-space GI: shaders/ssgi_blur.frag (depth-aware
//     bilateral with an a-trous `stepScale`) and shaders/ssgi_temporal.frag
//     (reprojection + depth disocclusion rejection).
// No GPL / id Tech / RBDOOM / Unreal source was consulted. See
// docs/CLEANROOM_PROCESS.md.
//
// WHY THIS FILE IS VULKAN-FREE: shaders/refl_denoise.comp is a line-for-line
// transcription of filterIteration() below. Keeping the filter's DEFINITION in
// plain C++ lets `--test-refldenoise` assert the three properties that actually
// matter — edge preservation, energy conservation and off-is-identity — on
// synthetic buffers with no GPU, no device and no swapchain, and lets a NEGATIVE
// CONTROL (the same loop with the edge weights removed) be run against the very
// same assertions to prove they can fail.
//
// ---------------------------------------------------------------------------
// WHAT IT IS FOR (the measured defect)
// ---------------------------------------------------------------------------
// shaders/refl.comp writes a half-res rgba16f buffer: rgb = reflected radiance,
// a = CONFIDENCE in [0,1]. Nothing filtered it — mesh.frag consumed it raw,
// unlike the GI chain which has gather -> temporal -> blur -> apply.
//
// On the real hero car (CTR.glb, material CTR_Body: base rough 0.4 / metal 0.8)
// that raw buffer reads as blotchy, streaky mottling on the door and rear
// quarter. Measured on flat door skin as mean |px - 9x9 local mean|:
// reflections off 5.53 -> shipped 7.69 (+39% high-frequency blotch). Sweeping
// mesh.frag's consumer-side blur disc barely moved it (7.70 / 7.92 / 7.69 /
// 7.56 across radii 0 / 6 / 14 / 24), which is the evidence that the noise is
// ALREADY IN THE BUFFER and the consumer kernel only softens it.
//
// TWO STRUCTURAL SOURCES, both of which this filter targets:
//
//  1. CONFIDENCE IS A BINARY DECISION PER PIXEL. refl.comp either finds a march
//     hit (confidence = edgeFade*backFade) or it does not (confidence 0, colour
//     BLACK), with two more discrete tiers for the ray-query fallback (0.65 and
//     0.45). On a curved glossy panel neighbouring pixels have wildly divergent
//     reflection rays, so that decision flips pixel to pixel and the confidence
//     field itself is the blotch. A flat test plate cannot show this: one flat
//     reflector has coherent neighbouring normals.
//     -> The filter accumulates PREMULTIPLIED (weight * confidence * radiance)
//        and normalises by the accumulated confidence, so a zero-confidence
//        neighbour contributes NO black to the colour, only to the confidence.
//        Averaging colour and confidence independently — which is what the
//        consumer disc in mesh.frag does — darkens instead.
//
//  2. THE RECONSTRUCTED NORMAL IS NOISY. refl.comp derives its geometric normal
//     from finite differences of the HALF-RES depth buffer, so on curved body
//     panels the reflected direction jitters, and with it the march result.
//     -> Wide spatial support (a-trous dilation reaches +-14 half-res texels in
//        3 iterations for 3 * 25 taps, where a plain box of that radius would
//        cost 841) averages that jitter out.
//
// And the second reported defect — reflection bleeding past the car's lower
// silhouette onto the floor in blocky stair-steps — is precisely what the DEPTH
// and NORMAL edge-stopping weights exist to prevent: they are what makes this a
// denoiser rather than a blur, and they are what the negative control removes.
//
// ---------------------------------------------------------------------------
// ROUGHNESS (the honest answer)
// ---------------------------------------------------------------------------
// Material roughness is NOT available here and is not packed into the buffer.
// refl.comp is a depth-only compute pass: it reconstructs position and normal
// from the depth buffer and has no material binding at all. This is a FORWARD
// renderer — there is no G-buffer carrying roughness, and the depth pre-pass
// writes depth only. Getting per-pixel roughness into the reflection buffer
// would mean adding a material-attribute render target to the pre-pass, which
// is a much larger architectural change than a denoise stage.
//
// So the roughness-proportional part of the filtering STAYS at the consumer,
// where roughness is already known per fragment: mesh.frag's existing
// roughness-driven disc. The split is:
//   * this pass          — wide, edge-aware, roughness-INDEPENDENT denoise that
//                          removes the confidence blotch and the normal jitter;
//   * mesh.frag's disc   — the roughness-proportional lobe widening, narrowed
//                          (kReflDiscScaleDenoised) because the buffer is no
//                          longer noisy, which ALSO shrinks the un-depth-tested
//                          halo that causes the silhouette bleed;
//   * mesh.frag's mirror early-out (rough <= 0.05) — reads the RAW buffer, so a
//                          mirror is never denoised at all. That is how "a
//                          mirror needs no denoise" is honoured without a
//                          roughness channel: the CONSUMER, which knows the
//                          roughness, chooses the buffer.
#pragma once

#include <cstdint>

namespace x3::refldn {

// A-trous 5x5 B3-spline row: the standard [1 4 6 4 1]/16 separable kernel, used
// here as an outer product (the 2D 5x5 weights). Exposed so the shader and the
// test share ONE definition of the spatial term.
inline constexpr float kAtrousRow[5] = {
    1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f
};

// Tunables. Defaults are the shipped values (r_refldenoise / r_refldn_*).
struct Params {
    // A-trous iterations. Tap spacing DOUBLES each iteration (1, 2, 4, ...), so
    // n iterations reach +-2*(2^n - 1) texels for n*25 taps. 0 = OFF: filter()
    // is then the identity and the renderer skips the passes entirely, which is
    // what makes `r_refldenoise 0` bit-exact to the pre-denoise build.
    //
    // 4 IS THE SHIPPED DEFAULT, chosen by a capture sweep on the hero car rather
    // than by taste. Door-skin blotch (mean |px - 9x9 local mean|) on
    // car_day_profile, against reflections-OFF 5.31 and shipped-raw 7.45:
    //     3 iterations -> 6.87   still visibly cloudy
    //     4 iterations -> 4.82   clean, and the broad reflected highlight sweep
    //                            across the panel is still clearly present
    //     5 iterations -> 3.44   BELOW the reflections-off baseline; on screen
    //                            the panel has gone flat and featureless. That
    //                            is the failure mode a metric on its own would
    //                            have walked straight into, and the reason the
    //                            screenshots are the gate and not the number.
    int   iterations = 4;
    // Depth edge stop. The tolerance is RELATIVE to view distance (the
    // ssgi_temporal.frag precedent: far surfaces have coarser depth and must not
    // be over-rejected), i.e. tolerance = depthSigma * dist.
    float depthSigma = 0.06f;
    // Normal edge stop exponent, pow(max(dot(n0,nt),0), normalPow). DELIBERATELY
    // PERMISSIVE: a car panel curves continuously and we WANT to filter across
    // that curvature (it is where the ray divergence, and therefore the noise,
    // lives). 16 keeps ~55% weight at 20 degrees apart and ~2% at 45, which cuts
    // creases and silhouettes without freezing the filter on smooth curvature.
    float normalPow  = 16.0f;
};

// One half-res reflection texel: refl.comp's rgba16f output.
struct Pixel {
    float rgb[3] = { 0.0f, 0.0f, 0.0f };
    float conf   = 0.0f;      // a = confidence [0,1]
};

// The per-texel geometry the edge stops read — refl.comp's AUX output
// (rgba16f: rgb = world normal, a = view distance). dist <= 0 means SKY or an
// early-out pixel with no valid surface; such taps are rejected outright.
struct Aux {
    float n[3] = { 0.0f, 0.0f, 0.0f };
    float dist = 0.0f;
};

// ONE a-trous iteration at tap spacing `step`. This is the function
// shaders/refl_denoise.comp transcribes; keep the two in sync.
//   src/aux : w*h texels, row-major, y-major
//   dst     : w*h texels (must not alias src)
void filterIteration(const Pixel* src, const Aux* aux, int w, int h,
                     int step, const Params& p, Pixel* dst);

// The NEGATIVE CONTROL, kept permanently: the identical loop with the depth and
// normal edge stops REMOVED (a plain 5x5 a-trous box). --test-refldenoise runs
// the edge-preservation assertions against this too and requires that they FAIL,
// which is what proves those assertions have teeth.
void filterIterationNoEdgeStops(const Pixel* src, const Aux* aux, int w, int h,
                                int step, const Params& p, Pixel* dst);

// Full chain: p.iterations passes with the spacing doubling each time, ping-pong
// between dst and scratch. Leaves the result in `dst`. iterations <= 0 copies
// src to dst VERBATIM (the r_refldenoise 0 contract).
// `edgeStops = false` selects the negative control for every iteration.
void filter(const Pixel* src, const Aux* aux, int w, int h, const Params& p,
            Pixel* dst, Pixel* scratch, bool edgeStops = true);

} // namespace x3::refldn
