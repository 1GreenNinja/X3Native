#version 450

// IBL specular prefilter: GGX importance-sample the env cubemap into a roughness
// MIP chain. Each mip = one roughness level (mip0 = mirror, last mip = rough).
// mesh.frag samples this with mip = roughness*maxMip for the specular IBL term.
// CLEAN-ROOM from the public Karis/Epic UE4 split-sum + learnopengl prefilter.

layout(set = 0, binding = 0) uniform samplerCube uEnv;

layout(push_constant) uniform Push {
    vec4 faceFwd;     // per-face direction basis
    vec4 faceRight;
    vec4 faceUp;
    vec4 misc;        // x = roughness for this mip, y = env base resolution (texels/face edge)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), radicalInverseVdC(i)); }

vec3 importanceSampleGGX(vec2 xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    vec3 H = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    return normalize(T * H.x + B * H.y + N * H.z);
}
float distributionGGX(float NoH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NoH * NoH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

void main() {
    vec2 fc = vUV * 2.0 - 1.0;
    vec3 N = normalize(pc.faceFwd.xyz + fc.x * pc.faceRight.xyz + fc.y * pc.faceUp.xyz);
    vec3 R = N;
    vec3 V = N;  // assume V = R = N (the standard split-sum prefilter simplification)

    float roughness = clamp(pc.misc.x, 0.0, 1.0);
    float resolution = max(pc.misc.y, 1.0);

    // Mip0 / mirror: just copy the env (no scatter), keeps sharp reflections crisp.
    if (roughness < 0.001) {
        outColor = vec4(textureLod(uEnv, N, 0.0).rgb, 1.0);
        return;
    }

    const uint kSamples = 256u;
    vec3 prefiltered = vec3(0.0);
    float totalWeight = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        vec2 xi = hammersley(i, kSamples);
        vec3 H  = importanceSampleGGX(xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        float NoL = max(dot(N, L), 0.0);
        if (NoL > 0.0) {
            // Mip-bias the env sample by the sample's solid angle vs a texel's, so
            // rough lobes pull from the env's lower mips (anti-fireflies, Karis).
            float NoH = max(dot(N, H), 0.0);
            float D   = distributionGGX(NoH, roughness);
            float pdf = (D * NoH / (4.0 * max(dot(V, H), 1e-4))) + 1e-4;
            float saTexel  = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(kSamples) * pdf + 1e-4);
            float mipBias  = roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);
            prefiltered += textureLod(uEnv, L, max(mipBias, 0.0)).rgb * NoL;
            totalWeight += NoL;
        }
    }
    outColor = vec4(prefiltered / max(totalWeight, 1e-4), 1.0);
}
