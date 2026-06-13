#version 450

// IBL diffuse irradiance convolution: for each texel of a small (~32px) cube
// face, integrate the env cubemap over the cosine-weighted hemisphere around the
// face normal. The result, divided by PI, is the diffuse irradiance E(N) used as
// the diffuse-IBL term in mesh.frag:  diffuseIBL = irradiance(N) * albedo * ao.
// CLEAN-ROOM from the public learnopengl IBL irradiance-convolution method.

layout(set = 0, binding = 0) uniform samplerCube uEnv;

layout(push_constant) uniform Push {
    vec4 faceFwd;
    vec4 faceRight;
    vec4 faceUp;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

void main() {
    vec2 fc = vUV * 2.0 - 1.0;
    vec3 N = normalize(pc.faceFwd.xyz + fc.x * pc.faceRight.xyz + fc.y * pc.faceUp.xyz);

    // Tangent frame around N.
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    vec3 irradiance = vec3(0.0);
    float nSamples = 0.0;
    const float dPhi   = 0.025;   // ~250 phi steps
    const float dTheta = 0.05;    // ~31 theta steps  -> ~7800 samples
    for (float phi = 0.0; phi < 2.0 * PI; phi += dPhi) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += dTheta) {
            // spherical (tangent space) -> world
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            // cos(theta)*sin(theta): the sin is the solid-angle weight, cos is Lambert.
            irradiance += textureLod(uEnv, sampleVec, 0.0).rgb * cos(theta) * sin(theta);
            nSamples += 1.0;
        }
    }
    // Riemann sum of the hemisphere integral; PI cancels into the diffuse BRDF in mesh.frag,
    // so store PI * mean(samples) (the standard convolved irradiance).
    irradiance = PI * irradiance / max(nSamples, 1.0);
    outColor = vec4(irradiance, 1.0);
}
