#version 450

// Bloom progressive DOWNSAMPLE (CLEAN-ROOM, original).
//
// References: Jimenez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare" (SIGGRAPH 2014) dual-filter downsample; Karis "partial
// Karis average" firefly suppression (the public next-gen-post writeups). NO
// game-engine source consulted.
//
// A 13-tap downsample filter (a center box of 4 + an outer ring of 8 + corners)
// that halves resolution with strong stability (no shimmering on bright thin
// fixtures). The FIRST mip additionally applies a soft-knee BRIGHT-PASS so only
// HDR radiance above the threshold enters the bloom chain; subsequent mips just
// downsample what is already in the chain.
//
// Push constant carries 1/srcResolution (the texel size of the texture we sample)
// plus the threshold/knee and a flag selecting the bright-pass on mip 0.

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    vec2  srcTexel;   // 1.0 / source resolution
    float threshold;  // bright-pass luminance threshold (linear)
    float knee;       // soft knee width around the threshold
    float intensity;  // unused here (kept for a shared push layout)
    int   firstPass;  // 1 = apply the bright-pass (mip 0), 0 = plain downsample
    float pad0, pad1;
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

// Soft-knee bright-pass (quadratic threshold curve). Keeps a smooth ramp into
// the bloom instead of a hard cutoff that pops as fixtures brighten.
vec3 brightPass(vec3 c) {
    float l = max(c.r, max(c.g, c.b));      // max-channel "brightness"
    float k = max(pc.knee, 1e-4);
    // soft knee: 0 below (threshold-knee), smooth ramp to full at threshold
    float soft = clamp(l - pc.threshold + k, 0.0, 2.0 * k);
    soft = (soft * soft) / (4.0 * k + 1e-4);
    float contrib = max(soft, l - pc.threshold);
    contrib = contrib / max(l, 1e-4);
    return c * contrib;
}

void main() {
    vec2 t = pc.srcTexel;
    // 13 taps: outer ring (a..h), inner box (i..l), center (e).
    vec3 a = texture(srcTex, vUV + t * vec2(-2.0,  2.0)).rgb;
    vec3 b = texture(srcTex, vUV + t * vec2( 0.0,  2.0)).rgb;
    vec3 c = texture(srcTex, vUV + t * vec2( 2.0,  2.0)).rgb;

    vec3 d = texture(srcTex, vUV + t * vec2(-2.0,  0.0)).rgb;
    vec3 e = texture(srcTex, vUV + t * vec2( 0.0,  0.0)).rgb;
    vec3 f = texture(srcTex, vUV + t * vec2( 2.0,  0.0)).rgb;

    vec3 g = texture(srcTex, vUV + t * vec2(-2.0, -2.0)).rgb;
    vec3 h = texture(srcTex, vUV + t * vec2( 0.0, -2.0)).rgb;
    vec3 i = texture(srcTex, vUV + t * vec2( 2.0, -2.0)).rgb;

    vec3 j = texture(srcTex, vUV + t * vec2(-1.0,  1.0)).rgb;
    vec3 k = texture(srcTex, vUV + t * vec2( 1.0,  1.0)).rgb;
    vec3 l = texture(srcTex, vUV + t * vec2(-1.0, -1.0)).rgb;
    vec3 m = texture(srcTex, vUV + t * vec2( 1.0, -1.0)).rgb;

    if (pc.firstPass == 1) {
        // Partial Karis average (Jimenez): weight each of the 5 sub-quads by the
        // inverse of its average luminance so a single hot firefly does not
        // dominate the downsample. Apply the bright-pass to the result.
        vec3 g0 = (j + k + l + m) * 0.25;          // center box
        vec3 g1 = (a + b + d + e) * 0.25;          // top-left
        vec3 g2 = (b + c + e + f) * 0.25;          // top-right
        vec3 g3 = (d + e + g + h) * 0.25;          // bottom-left
        vec3 g4 = (e + f + h + i) * 0.25;          // bottom-right
        float w0 = 1.0 / (1.0 + max(g0.r, max(g0.g, g0.b)));
        float w1 = 1.0 / (1.0 + max(g1.r, max(g1.g, g1.b)));
        float w2 = 1.0 / (1.0 + max(g2.r, max(g2.g, g2.b)));
        float w3 = 1.0 / (1.0 + max(g3.r, max(g3.g, g3.b)));
        float w4 = 1.0 / (1.0 + max(g4.r, max(g4.g, g4.b)));
        vec3 col = (g0*w0 + g1*w1 + g2*w2 + g3*w3 + g4*w4)
                 / max(w0 + w1 + w2 + w3 + w4, 1e-4);
        outColor = vec4(brightPass(col), 1.0);
    } else {
        // Standard weighted 13-tap downsample (no Karis needed deeper in the
        // chain where values are already smoothed).
        vec3 col = e * 0.125;
        col += (a + c + g + i) * 0.03125;
        col += (b + d + f + h) * 0.0625;
        col += (j + k + l + m) * 0.125;
        outColor = vec4(col, 1.0);
    }
}
