#version 450

// 2D HUD fragment: sample the bound texture (1x1 white for solid quads, or the
// 8x8 bitmap font atlas for text) and modulate by the per-vertex color. Alpha
// blended over the 3D scene; no depth.
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hudTex;

void main() {
    vec4 texel = texture(hudTex, vUV);
    outColor = texel * vColor;
}
