#version 450

// Bloom progressive UPSAMPLE (CLEAN-ROOM, original).
//
// Reference: Jimenez "Next Generation Post Processing in Call of Duty" (SIGGRAPH
// 2014) — a 3x3 tent filter upsample, accumulated from the smallest mip back up
// the chain so the bloom kernel is wide + smooth without a giant single blur. NO
// game-engine source consulted.
//
// This pass samples the SMALLER mip with a 9-tap tent and is ADDITIVELY blended
// (pipeline blend = ONE,ONE) onto the next-larger mip's existing content, so the
// driver does the "combine"; the shader just emits the filtered smaller mip. The
// per-mip `intensity` scales how much each upsample contributes (a slight <1
// keeps the chain from over-brightening).

layout(set = 0, binding = 0) uniform sampler2D srcTex;  // the SMALLER mip

layout(push_constant) uniform Push {
    vec2  srcTexel;   // 1.0 / source (smaller-mip) resolution
    float threshold;  // unused here
    float knee;       // unused here
    float intensity;  // contribution scale for this upsample step
    int   firstPass;  // unused here
    float pad0, pad1;
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 t = pc.srcTexel;
    // 3x3 tent: corners 1, edges 2, center 4 (sum 16).
    vec3 sum = vec3(0.0);
    sum += texture(srcTex, vUV + t * vec2(-1.0,  1.0)).rgb * 1.0;
    sum += texture(srcTex, vUV + t * vec2( 0.0,  1.0)).rgb * 2.0;
    sum += texture(srcTex, vUV + t * vec2( 1.0,  1.0)).rgb * 1.0;

    sum += texture(srcTex, vUV + t * vec2(-1.0,  0.0)).rgb * 2.0;
    sum += texture(srcTex, vUV + t * vec2( 0.0,  0.0)).rgb * 4.0;
    sum += texture(srcTex, vUV + t * vec2( 1.0,  0.0)).rgb * 2.0;

    sum += texture(srcTex, vUV + t * vec2(-1.0, -1.0)).rgb * 1.0;
    sum += texture(srcTex, vUV + t * vec2( 0.0, -1.0)).rgb * 2.0;
    sum += texture(srcTex, vUV + t * vec2( 1.0, -1.0)).rgb * 1.0;

    sum *= (1.0 / 16.0);
    outColor = vec4(sum * pc.intensity, 1.0);
}
