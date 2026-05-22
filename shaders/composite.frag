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

layout(push_constant) uniform Push {
    float bloomIntensity;  // additive bloom strength (0 = off)
    float exposure;        // pre-tonemap exposure multiplier
    float pad0, pad1;
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
    vec3 hdr   = texture(sceneTex, vUV).rgb;
    vec3 bloom = texture(bloomTex, vUV).rgb;
    vec3 color = hdr + bloom * pc.bloomIntensity;   // additive glow in linear light
    color = tonemapACES(color * pc.exposure);
    outColor = vec4(color, 1.0);
}
