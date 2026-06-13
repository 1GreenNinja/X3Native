#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_atmosphere.frag  — FORGE3D Planets HD / Atmosphere (fresnel glow shell)
//  ADDITIVE (Blend One One), ZWrite Off, Cull Back, emissive-only. Drawn on a
//  SHELL mesh slightly larger than the planet (planet.vert inflates verts along
//  the normal by uVertexOffset*0.1). A wide scattering halo + tight inner glow
//  ring, both gated by analytic fresnel masks, modulated by a day/night term.
//  NO noise, NO time — pure fresnel + one gradient-ramp lookup.
//
//  PIPELINE: blend = ADDITIVE (src ONE, dst ONE); depthWrite OFF; cull BACK.
//  Output the emissive RGB; alpha is ignored under One/One.
// =============================================================================

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec3 vObjPos;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec3 vViewDir;
layout(location = 5) in mat3 vTBN;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform PlanetFrame {
    vec3 uSunDir;   float uTime;
    vec3 uCamPos;   float _pf0;
    vec3 uLightColor; float _pf1;
    vec3 uAmbient;    float _pf2;
} F;

layout(set = 2, binding = 0) uniform PlanetParams {
    float uScatteringOffset; vec4 uScatteringColor; float uScatteringIntensity; float uScatteringFactor; float uUVOffset;
    float uGlowOffset; vec4 uGlowColor; float uGlowIntensity; float uGlowFactor;
    float uLightMultiply; float uLightExp;
    float uLightAtten;   // shadow/range atten (1.0 if unshadowed)
    float uVertexOffset; // used in planet.vert (shell inflation); here for completeness
} P;

layout(set = 3, binding = 0) uniform sampler2D _AtmosphereSample; // horizon gradient ramp (CLAMP_TO_EDGE)

void main() {
    vec3 N = normalize(vWorldNormal);
    vec3 V = normalize(F.uCamPos - vWorldPos); // toward camera
    vec3 L = normalize(F.uSunDir);

    float ndv    = dot(N, V);
    float absNdv = abs(ndv);
    float satNdv = clamp(ndv, 0.0, 1.0);

    // Near-binary analytic gates (*1000 then saturate).
    float offsetScattering = clamp(((P.uScatteringOffset / 10.0) + absNdv) * 1000.0, 0.0, 1.0);
    float offsetInnerRing  = clamp((absNdv + (P.uGlowOffset / 10.0)) * 1000.0, 0.0, 1.0);

    // Wide scattering halo (gradient ramp indexed by rim).
    vec2  scatterUV  = vec2(P.uUVOffset + satNdv);
    vec4  scatterTex = texture(_AtmosphereSample, scatterUV) * P.uScatteringColor;
    float scatterFalloff = pow(1.0 - satNdv, P.uScatteringFactor);
    vec4  scatterMap = vec4(offsetScattering * scatterFalloff * P.uScatteringIntensity) * scatterTex;

    // Tight inner glow ring.
    float glowFalloff = pow(1.0 - satNdv, P.uGlowFactor);
    vec4  innerRing   = offsetInnerRing * clamp(vec4(glowFalloff) * P.uGlowColor * P.uGlowIntensity, 0.0, 1.0);

    // Day/night light term (+ back-limb forward-scatter).
    float ndl     = dot(L, N);
    float backLimb = dot(-L, V);
    float litRaw  = ndl + (max(backLimb, -0.22) * P.uLightMultiply);
    float lightTerm = clamp(pow(clamp(litRaw, 0.0, 1.0), P.uLightExp), 0.0, 1.0);

    vec3 atmos = (scatterMap.rgb + innerRing.rgb) * lightTerm * P.uLightAtten * F.uLightColor;
    fragColor = vec4(atmos, 1.0); // ADDITIVE: alpha unused
}
