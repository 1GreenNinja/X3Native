#version 450
#extension GL_EXT_nonuniform_qualifier : require

// GPU-driven mesh vertex shader (Subsystem D).
//
// No per-draw push constants. Per-object data lives in a per-frame SSBO indexed
// by gl_InstanceIndex (the indirect draw's firstInstance + instance), and the
// camera viewProj is a single frame UBO. The fragment shader samples a bindless
// texture array by the per-object texIndex carried through here.

struct ObjectData {
    mat4 model;
    vec4 baseColorFactor;
    uint texIndex;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

layout(std430, set = 1, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
} cam;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTexIndex;
layout(location = 3) flat out vec4 vFactor;

void main() {
    ObjectData o = objBuf.objects[gl_InstanceIndex];
    gl_Position = cam.viewProj * o.model * vec4(inPos, 1.0);
    vNormal = mat3(o.model) * inNormal;
    vUV = inUV;
    vTexIndex = o.texIndex;
    vFactor = o.baseColorFactor;
}
