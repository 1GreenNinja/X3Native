#ifndef X3_MESH_REFLECTIONS_GLSL
#define X3_MESH_REFLECTIONS_GLSL
// ---- GLOSSY (roughness-aware) reflection sampling -------------------------
// The reflection pass traces ONE mirror ray per pixel, so a single tap gives
// every surface a chrome-sharp reflection. The previous consumer compensated by
// FADING reflections out entirely as roughness rose (gone by ~0.6), which meant
// brushed metal, satin paint and polished concrete got NO traced reflection at
// all -- only the low-res prefiltered env cube.
//
// Instead, widen a small disc of taps with roughness: a mirror surface still
// takes exactly ONE tap (bit-identical to the old path at rough <= 0.05), and
// rougher surfaces get a genuinely BLURRED reflection, which is what those
// materials actually look like. Golden-angle spiral so the taps decorrelate
// without a LUT; TAA integrates the residual.
//
// NOTE: `texel` is the FULL-RES pixel size (ssao.ctrl.zw). When the reflection
// buffer is half-res its own texels are twice this, so the disc is a
// conservative UNDER-estimate of the true blur radius -- deliberately, since
// over-blurring reads as a smear while under-blurring merely reads as sharper.
// RADIUS: kept at 14 after an eyeball A/B sweep of 6 / 14 / 24 on the
// --screenshot-reflverify rig (docs/screenshots/rt-refl-verify). 6 is clearly too
// small -- it is WORSE than no blur at all, because a disc that narrow just
// duplicates a thin feature instead of dissolving it. 24 is visibly the smoothest
// (horizontal-banding energy -52% vs 14) BUT it also widened the halo bleeding
// across a depth silhouette by +16%, which is exactly the smear the UNDER-estimate
// above is deliberately guarding against -- so 14 stays.
const float kReflBlurPx = 14.0;   // disc radius in full-res pixels at rough = 1
vec4 sampleReflGlossy(vec2 uv, float rough, vec2 texel) {
    vec4 c = texture(reflTex, uv);
    if (rough <= 0.05) return c;              // mirror: one tap, unchanged
    const int   kTaps = 6;
    const float kGolden = 2.39996323;         // radians
    float r = rough * kReflBlurPx;
    // PER-PIXEL ROTATION of the whole disc (interleaved-gradient noise).
    // The golden-angle spiral decorrelates the taps from EACH OTHER, but without
    // this every pixel used the IDENTICAL 6 offsets, so the kernel behaved as a
    // fixed 6-point comb: a thin bright reflected feature came out as 6 hard
    // shifted GHOSTS (venetian-blind banding) rather than a blur, and being
    // static it was something TAA could never integrate away. Rotating per pixel
    // scatters the ghosts into a soft lobe. Measured on the reflverify rig:
    // banding energy -10% and depth-edge halo -10% vs the fixed kernel, for ~2
    // ALU and no change at all to the sampling footprint.
    float ign   = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float phase = 6.2831853 * ign;
    for (int i = 0; i < kTaps; ++i) {
        float a  = float(i) * kGolden + phase;
        float rr = sqrt((float(i) + 0.5) / float(kTaps)) * r;
        c += texture(reflTex, uv + vec2(cos(a), sin(a)) * rr * texel);
    }
    return c / float(kTaps + 1);
}
// ---- DENOISED variant (r_refldenoise > 0; ssao.refl.w carries the disc scale) --
// A SEPARATE function, and sampleReflGlossy above is left textually VERBATIM, so
// the legacy path keeps its exact expression tree and the compiler has no reason
// to reschedule or re-contract it.
//
// WHY THAT MATTERS, MEASURED: the first version of this lane folded both paths
// into one function with a `* discScale` factor that is exactly 1.0 when the
// stage is off. Arithmetically a no-op — and it still moved ~0.17% of subpixels
// by +-1 LSB in the r_refldenoise 0 A/B against the pre-lane build, because the
// surrounding FMA contraction shifted. Visually nothing; enough to flip an md5
// gate, which is exactly what the fallback cvar exists to protect. (The same
// finding is why the denoise AUX buffer became its own pass, refl_aux.comp,
// instead of ~10 lines inside refl.comp.)
//
// TWO DELIBERATE DIFFERENCES FROM THE LEGACY PATH:
//
//  1. THE MIRROR EARLY-OUT STILL READS THE RAW BUFFER. This is how "a mirror
//     needs no denoise" is honoured WITHOUT a roughness channel in the
//     reflection buffer: the CONSUMER knows the per-fragment roughness, so the
//     consumer picks the buffer. CTR_Body's clearcoat lobe (roughness 0.05)
//     lands here and is therefore untouched by this lane — which matters,
//     because the clearcoat is the one part that was already correct.
//
//  2. THE DISC IS SCALED DOWN (ssao.refl.w = r_refldn_disc, default 0.4). The
//     14 px tuning was optimising the wrong term and the real-car measurement
//     proved it: sweeping that radius 0 / 6 / 14 / 24 moved the door-skin blotch
//     metric only 7.70 / 7.92 / 7.69 / 7.56, a +-5% spread against the 28% drop
//     from disabling reflections outright. The noise was in the BUFFER. With the
//     a-trous stage in front this disc no longer has to fight it — and shrinking
//     it is ALSO the fix for the second defect, the reflection halo bleeding
//     past the car's lower silhouette onto the floor, because THESE taps have no
//     depth test at all (there is no depth sampler in set 3) whereas the denoise
//     stage's taps carry depth AND normal edge stops. The wide averaging moves
//     to the pass that can reject across a silhouette; what stays here is only
//     the roughness-PROPORTIONAL lobe widening, the one part that genuinely
//     needs per-fragment roughness.
//
//  3. THE RAW -> DENOISED HAND-OFF IS A ROUGHNESS RAMP, NOT A SWITCH. This is
//     the whole of "roughness widens the filter", implemented at the ONLY place
//     that knows the roughness. The denoise pass itself is roughness-BLIND —
//     material roughness is not in the reflection buffer and cannot cheaply be
//     put there (forward renderer, no G-buffer, and refl.comp is a depth-only
//     pass with no material binding at all; see engine/rhi/ReflDenoise.h) — so a
//     fixed-width filter hits a near-mirror exactly as hard as matte paint.
//     THAT WAS NOT THEORETICAL: with a hard switch at 0.05 the showroom's
//     polished floor (rough 0.08 / metal 0.5) took the full a-trous treatment
//     and its mirror image of the wheel visibly lost its spokes in the A/B
//     captures, while the car's base lobe (rough 0.4) needed every bit of it.
//     The ramp gives roughness 0.08 about 4% denoise and 0.4 the full 100%.
const float kDenoiseRampLo = 0.05;   // <= this: pure raw (the mirror early-out)
const float kDenoiseRampHi = 0.30;   // >= this: fully denoised
vec4 sampleReflGlossyDenoised(vec2 uv, float rough, vec2 texel, float discScale) {
    vec4 raw = texture(reflTex, uv);
    if (rough <= kDenoiseRampLo) return raw;          // MIRROR: raw, undenoised
    vec4 c = texture(reflDnTex, uv);
    const int   kTaps = 6;
    const float kGolden = 2.39996323;         // radians
    float r = rough * kReflBlurPx * discScale;
    float ign   = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float phase = 6.2831853 * ign;
    for (int i = 0; i < kTaps; ++i) {
        float a  = float(i) * kGolden + phase;
        float rr = sqrt((float(i) + 0.5) / float(kTaps)) * r;
        c += texture(reflDnTex, uv + vec2(cos(a), sin(a)) * rr * texel);
    }
    c /= float(kTaps + 1);
    // ONE extra fetch (the raw centre tap above) buys the whole ramp; the disc
    // itself is never sampled twice.
    return mix(raw, c, smoothstep(kDenoiseRampLo, kDenoiseRampHi, rough));
}
// ONE call point for both, so the two consumers below stay symmetrical.
// ssao.refl.w is 0 exactly when the denoise stage did not run this frame.
vec4 sampleReflAuto(vec2 uv, float rough, vec2 texel) {
    return (ssao.refl.w > 0.0) ? sampleReflGlossyDenoised(uv, rough, texel, ssao.refl.w)
                               : sampleReflGlossy(uv, rough, texel);
}
#endif  // X3_MESH_REFLECTIONS_GLSL
