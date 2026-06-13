#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_thunderstorm.frag  — FORGE3D Planets HD / Thunderstorm (storm world)
//  Animated flow-distorted normal + detail (Tint Low<->High gradient), fresnel
//  rim, specular PBR, atmospheric scatter LUT, and EMISSIVE LIGHTNING:
//    triple clamped-sine flicker * triplanar mask (two scrolling layers) *
//    floor()-quantized triplanar strike (two layers) * lightning color.
//
//  final = saturate(scatter * surf) + max(0, lightningEmissive)
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
    float uNormalUVTiling; float uNormalUVSpeed; float uNormalTiling; float uNormalSpeed;
    float uNormalDistortionFactor; float uNormalScale;
    float uDistortionUVTiling; float uDistortionUVSpeed; float uDistortionTiling; float uDistortionSpeed; float uDistortionFactor;
    vec4  uTintHigh; vec4 uTintLow; float uDetailPow; float uDetailBoost;
    vec4  uSpecularColor; float uSpecular; float uSmoothness;
    vec4  uScatterColor; float uScatterBoost; float uScatterIndirect; float uScatterStretch; float uScatterCenterShift;
    float uFresnelMult; float uFresnelPower; vec4 uFresnelColor;
    float uLightingMaskATiling; float uLightingMaskBTiling; float uLightingMaskAUVSpeed; float uLightingMaskBUVSpeed;
    float uLightingATiling; float uLightingBTiling; float uLightingAFrequency; float uLightingBFrequency;
    float uLightningSineMult; float uLightningMaskPow; float uLightningBoost; vec4 uLightningColor;
} P;

layout(set = 3, binding = 0) uniform sampler2D _NormalMap;
layout(set = 3, binding = 1) uniform sampler2D _DistortionMap;
layout(set = 3, binding = 2) uniform sampler2D _DistortionUVMap;
layout(set = 3, binding = 3) uniform sampler2D _ScatterMap;
layout(set = 3, binding = 4) uniform sampler2D _LightingMaskMap;
layout(set = 3, binding = 5) uniform sampler2D _LightingMap;

void main() {
    vec3 oN = normalize(vObjPos);
    vec3 oP = vObjPos;
    vec3 N  = normalize(vWorldNormal);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(F.uSunDir);

    // ---- Animated normal (UV self-distortion) ----
    vec2 nWarp = flowDistortOffset(_NormalMap, oP, oN, P.uNormalUVTiling, 5.0,
                                   vec2(F.uTime * P.uNormalUVSpeed), P.uNormalDistortionFactor);
    vec2 nOffset = vec2(F.uTime * P.uNormalSpeed) + nWarp;
    vec4 nTri = clamp(triplanar(_NormalMap, oP, oN, P.uNormalTiling, nOffset), 0.0, 1.0);
    vec3 tN = unpackScaleNormal(nTri, P.uNormalScale);
    vec3 Npert = normalize(vTBN * tN);

    // ---- Animated detail -> Tint gradient ----
    vec2 dWarp = flowDistortOffset(_DistortionUVMap, oP, oN, P.uDistortionUVTiling, 5.0,
                                   vec2(F.uTime * P.uDistortionUVSpeed), P.uDistortionFactor);
    vec2 dOffset = vec2(F.uTime * P.uDistortionSpeed) + dWarp;
    vec4 detailTri = clamp(triplanar(_DistortionMap, oP, oN, P.uDistortionTiling, dOffset), 0.0, 1.0);
    vec4 detail = saturate4(pow(detailTri, vec4(P.uDetailPow)) * P.uDetailBoost);
    vec3 tint = saturate3(mix(P.uTintLow.rgb, P.uTintHigh.rgb, detail.r));

    float fres = saturate1(pow(saturate1(1.0 - max(dot(V, Npert), 0.0)), P.uFresnelPower)) * P.uFresnelMult;
    vec3 albedo = fres * P.uFresnelColor.rgb + tint;
    vec3 specColor = P.uSpecularColor.rgb * detail.rgb * P.uSpecular;

    vec3 surf = standardSpecular(albedo, specColor, P.uSmoothness, Npert, V, L, F.uLightColor, F.uAmbient);

    // ---- Scatter (geometric normal) ----
    vec3 scatter = scatterTerm(_ScatterMap, N, L, V, P.uScatterCenterShift, P.uScatterStretch,
                               P.uScatterColor.rgb, F.uLightColor, P.uScatterBoost, P.uScatterIndirect);
    vec3 lit = saturate3(scatter * surf);

    // ---- Lightning ----
    float s1 = clamp(sin(F.uTime * (2.0  * PI)), 0.3, 1.0);
    float s2 = clamp(sin(F.uTime * (5.0  * PI)), 0.5, 1.0);
    float s3 = clamp(sin(F.uTime * (10.0 * PI)), 0.7, 1.0);
    float sine = s1 * s2 * s3;

    vec4 maskA = clamp(triplanar(_LightingMaskMap, oP, oN, P.uLightingMaskATiling, vec2(F.uTime*P.uLightingMaskAUVSpeed)), 0.0, 1.0);
    vec4 maskB = clamp(triplanar(_LightingMaskMap, oP, oN, P.uLightingMaskBTiling, vec2(F.uTime*P.uLightingMaskBUVSpeed)), 0.0, 1.0);
    vec4 lightningMask = maskA * maskB;
    vec4 maskSine = saturate4(pow(vec4(P.uLightningSineMult) * vec4(sine) * lightningMask, vec4(P.uLightningMaskPow)));

    float strikeAOff = floor(F.uTime * P.uLightingAFrequency) * 0.9;
    float strikeBOff = 0.9 * floor(F.uTime * P.uLightingBFrequency);
    vec4 strikeA = clamp(triplanar(_LightingMap, oP, oN, P.uLightingATiling, vec2(strikeAOff)), 0.0, 1.0);
    vec4 strikeB = clamp(triplanar(_LightingMap, oP, oN, P.uLightingBTiling, vec2(strikeBOff)), 0.0, 1.0);
    float lightning = (strikeA * strikeB * P.uLightningBoost).r;

    vec3 lightningEmissive = max(vec3(0.0), maskSine.rgb * lightning * P.uLightningColor.rgb);

    fragColor = vec4(lit + lightningEmissive, 1.0); // HDR emissive -> bloom
}
