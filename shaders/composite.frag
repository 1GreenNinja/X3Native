#version 450

// HDR COMPOSITE + tonemap (CLEAN-ROOM, original).
//
// The final post pass: read the linear-HDR scene color (set0/binding0) and the
// blurred bloom texture (set0/binding1), additively combine the bloom (scaled by
// bloomIntensity), then apply the SAME ACES filmic curve (Narkowicz approx) that
// mesh.frag / sky.frag used to apply per-fragment — now applied EXACTLY ONCE
// here. Output is the LDR swapchain/offscreen image. This keeps the look
// consistent with the pre-bloom build (same tonemap) while adding a tasteful
// additive glow around emissive fixtures + bright lights.

layout(set = 0, binding = 0) uniform sampler2D sceneTex;  // linear HDR scene
layout(set = 0, binding = 1) uniform sampler2D bloomTex;   // blurred bloom (mip0)
// Auto-exposure result written by autoexposure.comp this frame (binding 2).
// Only sampled when aeEnabled != 0 (with AE off the chain never runs).
layout(set = 0, binding = 2) readonly buffer ExposureBuf {
    float adapted;   // adapted exposure multiplier (eye adaptation)
    float avgLog;    // average log2 scene luminance (debug)
    float pad0, pad1;
} ae;

layout(push_constant) uniform Push {
    float bloomIntensity;  // additive bloom strength (<= 0 = off; chain skipped)
    float exposure;        // exposure BIAS (r_exposure) on top of auto-exposure
    int   tonemapMode;     // 0 = passthrough clamp (debug A/B), 1 = ACES (default)
    int   aeEnabled;       // 1 = multiply in the adapted auto-exposure
    float sharpen;         // r_taasharpen: post-TAA sharpen amount (0 = OFF: the
                           // sharpen taps are never sampled -> byte-identical to
                           // the pre-sharpen composite). Forced 0 when TAA is off.
    float texelW;          // 1/extent for the sharpen cross taps
    float texelH;
    float vignette;        // cinematic edge-darkening [0..1]; 0 = off (no-op)
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

// ACES filmic tonemap (Narkowicz approximation) — IDENTICAL constants to the
// curve mesh.frag/sky.frag used before, so the composited look matches the prior
// build (no wash-out) while the tonemap now runs once on HDR+bloom.
vec3 tonemapACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 color = texture(sceneTex, vUV).rgb;
    // Post-TAA SHARPEN (RCAS-style, original): TAA's temporal accumulation costs
    // a little micro-contrast; a small unsharp restores it. The 4-neighbor cross
    // unsharp result is CLAMPED to the local min/max so it never rings or
    // amplifies HDR fireflies (the RCAS idea: sharpen within the local range).
    // sharpen == 0.0 takes the branch never -> identical to the pre-TAA composite.
    if (pc.sharpen > 0.0) {
        vec3 n = texture(sceneTex, vUV + vec2(0.0, -pc.texelH)).rgb;
        vec3 s = texture(sceneTex, vUV + vec2(0.0,  pc.texelH)).rgb;
        vec3 w = texture(sceneTex, vUV + vec2(-pc.texelW, 0.0)).rgb;
        vec3 e = texture(sceneTex, vUV + vec2( pc.texelW, 0.0)).rgb;
        vec3 mn = min(color, min(min(n, s), min(w, e)));
        vec3 mx = max(color, max(max(n, s), max(w, e)));
        vec3 sharp = color + (color * 4.0 - (n + s + w + e)) * 0.25 * pc.sharpen;
        color = clamp(sharp, mn, mx);
    }
    // Additive glow in linear light. Guarded so a DISABLED bloom (r_bloom 0 —
    // the chain didn't run this frame) never samples the untouched mip target.
    if (pc.bloomIntensity > 0.0)
        color += texture(bloomTex, vUV).rgb * pc.bloomIntensity;
    // Exposure: auto-adaptation (when enabled) x the r_exposure bias.
    float exposure = pc.exposure * ((pc.aeEnabled != 0) ? ae.adapted : 1.0);
    color *= exposure;
    // Tonemap: ACES (default) or a raw passthrough clamp for A/B debugging.
    if (pc.tonemapMode == 1) color = tonemapACES(color);
    else                     color = clamp(color, 0.0, 1.0);
    // Cinematic vignette (post-tonemap): smooth radial edge-darkening. 0 = no-op.
    if (pc.vignette > 0.0) {
        vec2 d = vUV - vec2(0.5);
        float r = dot(d, d);                         // squared radius from center
        float vig = 1.0 - pc.vignette * smoothstep(0.10, 0.62, r);
        color *= vig;
    }
    outColor = vec4(color, 1.0);
}
