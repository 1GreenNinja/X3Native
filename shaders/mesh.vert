#version 450

// Push constants: 2 mat4 = 128 bytes (the guaranteed minimum push range).
// baseColorFactor does NOT fit here, so it lives in a per-draw UBO (set0/binding1).
layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vNormal = mat3(pc.model) * inNormal;
    vUV = inUV;
}
