#version 450
#extension GL_EXT_nonuniform_qualifier : require

// GPU-driven mesh fragment shader (Subsystem D).
//
// Bindless: one large combined-image-sampler array at set0/binding0. The
// per-object texIndex (from the vertex stage) selects the texture; index 0 is
// the built-in 1x1 white default. baseColorFactor rides through from the SSBO
// row (no per-draw UBO).

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 3) flat in vec4 vFactor;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(0.4, 1.0, 0.3));
    float ndl = max(dot(N, L), 0.0);
    float light = 0.25 + 0.75 * ndl;          // ambient + directional diffuse

    vec4 albedo = texture(textures[nonuniformEXT(vTexIndex)], vUV) * vFactor;
    outColor = vec4(albedo.rgb * light, albedo.a);
}
