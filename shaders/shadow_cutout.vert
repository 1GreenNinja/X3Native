#version 450

// Directional-shadow depth vertex shader — ALPHA-CUTOUT variant.
//
// CLEAN-ROOM, original. shadow.vert has NO fragment stage, so an alphaMode==MASK
// billboard (a snow fir, a people sprite) casts the shadow of its FULL QUAD — on
// snow under a high sun that reads as a scatter of hard black RECTANGLES around
// every tree. This variant passes UV + texIndex + the base-color alpha factor to
// depth_cutout.frag (reused verbatim), which replicates mesh.frag's exact
// bit-31 / 0.5-threshold discard, so a fir casts a FIR-shaped shadow.
//
// Position math is bit-identical to shadow.vert (same struct stride, same
// lightViewProj * (model * pos) grouping) and it keeps the SAME visBuf
// indirection, so a mixed cutout/opaque shadow pass stays consistent under GPU
// culling. Engaged per draw-group only when the host opts in (setShadowCutout);
// every other world keeps the historical full-quad shadow bit-for-bit.

// MUST match the std430 stride of mesh.vert's ObjectData (160 B w/ glass params).
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
    vec4 glassParams;
    vec4 glassTint;
};

layout(std430, set = 0, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

layout(std430, set = 0, binding = 2) readonly buffer VisibleIdx {
    uint idx[];
} visBuf;

layout(set = 0, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
} cam;

// CASCADED SHADOW MAPS: the light matrix of the cascade currently being
// rasterized (see shaders/shadow.vert for why this is a push constant). The
// cutout pipeline layout declares the SAME range at the same offset/stage so
// recordShadowPassBody can re-push it when it swaps between the two pipelines.
layout(push_constant) uniform CascadePush {
    mat4 lightViewProj;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;   // unused; kept so the VBO layout matches
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out uint  vTexIndex;
layout(location = 2) flat out float vAlphaFactor;

void main() {
    ObjectData o = objBuf.objects[visBuf.idx[gl_InstanceIndex]];
    vec4 worldPos = o.model * vec4(inPos, 1.0);
    gl_Position   = pc.lightViewProj * worldPos;
    vUV           = inUV;
    vTexIndex     = o.texIndex;
    vAlphaFactor  = o.baseColorFactor.a;
}
