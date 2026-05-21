#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Per-material / per-draw resources (set 0).
//   binding 0: base-color texture (sRGB image -> sampling linearizes)
//   binding 1: per-draw factor UBO
layout(set = 0, binding = 0) uniform sampler2D baseColorTex;
layout(set = 0, binding = 1) uniform Factor {
    vec4 baseColorFactor;
} fc;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(0.4, 1.0, 0.3));
    float ndl = max(dot(N, L), 0.0);
    float light = 0.25 + 0.75 * ndl;          // ambient + directional diffuse

    vec4 albedo = texture(baseColorTex, vUV) * fc.baseColorFactor;
    outColor = vec4(albedo.rgb * light, albedo.a);
}
