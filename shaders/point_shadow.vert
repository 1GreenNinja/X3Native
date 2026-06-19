#version 450

// Gap 6 — point-light shadow depth-only vertex shader (atlas tiers 1-2).
//
// Renders scene depth from ONE cube face of ONE budgeted point-light caster into
// that face's tile in the shadow atlas. One draw call per (slot x face): the host
// sets the tile viewport and pushes that face's view-proj matrix as a push
// constant (the per-frame shadow table holds the same matrices for the fragment
// sampler). There is NO fragment shader (depth-only) — only gl_Position is set.
//
// Reuses the SAME per-object SSBO + indirect draw buffer (set 0) the sun shadow
// pass and the color pass use, indexed by gl_InstanceIndex, so the atlas sees the
// exact same geometry. Mirrors shaders/shadow.vert; the ONLY difference is that
// the transform arrives via a push constant (per-face) rather than the camera UBO.

// MUST match the std430 stride of mesh.vert's ObjectData (160 B) — same as
// shadow.vert. Only o.model is used here; the rest force the correct stride.
struct ObjectData {
    mat4 model;          // 64
    vec4 baseColorFactor;// 16
    vec4 emissive;       // 16
    uint texIndex;       // -- 8 uints = 32 B -> 128 B
    uint _pad0;
    uint _pad1;
    uint _pad2;
    uint _pad3;
    uint _pad4;
    uint _pad5;
    uint _pad6;
    vec4 glassParams;    // pad to match the 160 B C++ ObjectData std430 stride
    vec4 glassTint;
};

layout(std430, set = 0, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

// D15 GPU cull indirection (identity when cull off) — same as shadow.vert.
layout(std430, set = 0, binding = 2) readonly buffer VisibleIdx {
    uint idx[];
} visBuf;

// Per-face view-proj (90 deg FOV, near..range). One draw per face sets this.
layout(push_constant) uniform Push {
    mat4 faceViewProj;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;   // unused; kept so the VBO layout matches
layout(location = 2) in vec2 inUV;       // unused

void main() {
    ObjectData o = objBuf.objects[visBuf.idx[gl_InstanceIndex]];
    vec4 worldPos = o.model * vec4(inPos, 1.0);
    gl_Position = pc.faceViewProj * worldPos;
}
