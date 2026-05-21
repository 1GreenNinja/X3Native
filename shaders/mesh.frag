#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(0.4, 1.0, 0.3));
    float ndl = max(dot(N, L), 0.0);
    float light = 0.25 + 0.75 * ndl;   // ambient + diffuse (half-lambert-ish)
    outColor = vec4(vColor * light, 1.0);
}
