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
const float kReflBlurPx = 14.0;   // disc radius in full-res pixels at rough = 1
vec4 sampleReflGlossy(vec2 uv, float rough, vec2 texel) {
    vec4 c = texture(reflTex, uv);
    if (rough <= 0.05) return c;              // mirror: one tap, unchanged
    const int   kTaps = 6;
    const float kGolden = 2.39996323;         // radians
    float r = rough * kReflBlurPx;
    for (int i = 0; i < kTaps; ++i) {
        float a  = float(i) * kGolden;
        float rr = sqrt((float(i) + 0.5) / float(kTaps)) * r;
        c += texture(reflTex, uv + vec2(cos(a), sin(a)) * rr * texel);
    }
    return c / float(kTaps + 1);
}
#endif  // X3_MESH_REFLECTIONS_GLSL
