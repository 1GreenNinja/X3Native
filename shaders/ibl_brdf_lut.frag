#version 450

// IBL split-sum BRDF integration LUT (Karis/Epic UE4 "Real Shading in Unreal
// Engine 4"). Precomputed ONCE into a 256x256 RG16F texture: the .r channel is
// the F0 scale, the .g channel is the F0 bias of the environment BRDF, indexed
// by (u = NoV, v = roughness). mesh.frag's specular IBL term is then:
//   specular = prefiltered * (F0 * lut.r + lut.g)
// This is the standard, scene-independent half of split-sum IBL — clean-room
// from the public Epic course notes + learnopengl IBL article (no engine source).

layout(location = 0) in vec2 vUV;     // fullscreen.vert: [0,1] screen UV
layout(location = 0) out vec2 outRG;  // RG16F: x = scale, y = bias

const float PI = 3.14159265359;

// Van der Corput radical inverse (low-discrepancy Hammersley sequence).
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), radicalInverseVdC(i)); }

// GGX importance sample: map a 2D Hammersley point to a half-vector around N
// for roughness `a` (perceptual roughness here; squared inside).
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

// Smith geometry term for IBL (k uses the a/2 IBL remap, Karis).
float geometrySchlickGGX(float NoV, float roughness) {
    float a = roughness;
    float k = (a * a) / 2.0;
    return NoV / (NoV * (1.0 - k) + k);
}
float geometrySmith(float NoV, float NoL, float roughness) {
    return geometrySchlickGGX(NoV, roughness) * geometrySchlickGGX(NoL, roughness);
}

vec2 integrateBRDF(float NoV, float roughness) {
    vec3 V = vec3(sqrt(1.0 - NoV * NoV), 0.0, NoV);   // V in the tangent frame (N = +Z)
    float A = 0.0, B = 0.0;
    const uint kSamples = 1024u;
    vec3 N = vec3(0.0, 0.0, 1.0);
    for (uint i = 0u; i < kSamples; ++i) {
        vec2 xi = hammersley(i, kSamples);
        vec3 H  = importanceSampleGGX(xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        float NoL = max(L.z, 0.0);
        float NoH = max(H.z, 0.0);
        float VoH = max(dot(V, H), 0.0);
        if (NoL > 0.0) {
            float G    = geometrySmith(NoV, NoL, roughness);
            float Gvis = (G * VoH) / (NoH * NoV);
            float Fc   = pow(1.0 - VoH, 5.0);
            A += (1.0 - Fc) * Gvis;
            B += Fc * Gvis;
        }
    }
    return vec2(A, B) / float(kSamples);
}

void main() {
    // u = NoV, v = roughness. Clamp NoV off zero to avoid the degenerate edge tap.
    float NoV       = max(vUV.x, 1e-3);
    float roughness = vUV.y;
    outRG = integrateBRDF(NoV, roughness);
}
