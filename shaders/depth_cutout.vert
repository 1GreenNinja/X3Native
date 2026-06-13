#version 450

// Camera depth pre-pass vertex shader — ALPHA-CUTOUT variant (depth.vert +
// UV/texIndex/alpha-factor passthrough for the fragment-stage alpha test).
//
// CLEAN-ROOM, original. Used by the SSR/RT-reflections depth pre-pass for draw
// groups containing alphaMode==MASK instances (foliage / people billboards):
// the plain depth.vert pipeline has NO fragment stage, so it writes depth for
// the FULL billboard quad — the main color pass then alpha-discards those
// texels under its EQUAL depth test and nothing ever fills them (flat
// clear-color rectangles around every tree). This variant lets the paired
// fragment shader (depth_cutout.frag) replicate mesh.frag's exact cutout
// discard so the pre-pass depth matches the color pass texel-for-texel.
//
// MUST stay position-bit-identical with depth.vert AND mesh.vert (same struct
// stride, same multiply grouping) — the color pass depth-tests EQUAL.

struct ObjectData {
    mat4 model;          // 64
    vec4 baseColorFactor;// 16 (a multiplies the sampled alpha, like mesh.frag)
    vec4 emissive;       // 16
    uint texIndex;       // bits 30/31 = alpha-mode flags, low 30 = bindless index
    uint _pad0;
    uint _pad1;
    uint _pad2;
    uint _pad3;
    uint _pad4;
    uint _pad5;
    uint _pad6;
    vec4 glassParams;    // pad to match C++ 160 B ObjectData std430 stride
    vec4 glassTint;
};

layout(std430, set = 0, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

layout(set = 0, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
} cam;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;   // unused; kept so the VBO layout matches
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out uint  vTexIndex;
layout(location = 2) flat out float vAlphaFactor;

void main() {
    ObjectData o = objBuf.objects[gl_InstanceIndex];
    // Group the multiply EXACTLY as mesh.vert/depth.vert (worldPos first, then
    // viewProj) so the floating-point depth is bit-invariant and survives EQUAL.
    vec4 worldPos = o.model * vec4(inPos, 1.0);
    gl_Position = cam.viewProj * worldPos;
    vUV          = inUV;
    vTexIndex    = o.texIndex;
    vAlphaFactor = o.baseColorFactor.a;
}
