#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_moon.frag  — FORGE3D Planets HD / Moon (airless rocky body)
//  Object-space triplanar Albedo/Normal/Detail/Specular + detail-boost albedo +
//  fresnel rim + specular-workflow PBR + scatter/wrap-light tint. STATIC (no time).
//
//  albedo = saturate(detail + albedo*Tint + fresnel)
//  final  = saturate(scatter * standardSpecular(...))
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
    float uAlbedoTiling; float uNormalTiling; float uDetailTiling; float uSpecularTiling;
    vec4  uTint; vec4 uDetailTint; float uDetailBoost; float uDetailPow;
    float uNormalScale;
    float uFresnelPower; float uFresnelMult; vec4 uFresnelColor;
    float uSpecular; vec4 uSpecularTint; float uSmoothness;
    vec4  uScatterColor; float uScatterBoost; float uScatterIndirect; float uScatterStretch; float uScatterCenterShift;
} P;

layout(set = 3, binding = 0) uniform sampler2D _Albedo;
layout(set = 3, binding = 1) uniform sampler2D _Normal;
layout(set = 3, binding = 2) uniform sampler2D _DetailMap;
layout(set = 3, binding = 3) uniform sampler2D _SpecularMap;
layout(set = 3, binding = 4) uniform sampler2D _ScatterMap;

void main() {
    vec3 oN = normalize(vObjPos);
    vec3 N  = normalize(vWorldNormal);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(F.uSunDir);
    mat3 w2t = transpose(vTBN);

    vec4 triDetail = triplanar(_DetailMap,   vObjPos, oN, 5.0, P.uDetailTiling);
    vec4 triAlbedo = triplanar(_Albedo,      vObjPos, oN, 5.0, P.uAlbedoTiling);
    vec4 triNormal = triplanar(_Normal,      vObjPos, oN, 5.0, P.uNormalTiling);
    vec4 triSpec   = triplanar(_SpecularMap, vObjPos, oN, 5.0, P.uSpecularTiling);

    vec3 nTan = normalize(unpackScaleNormal(triNormal, P.uNormalScale));
    vec3 shadeN = normalize(vTBN * nTan);

    // Fresnel uses tangent-space view dir vs tangent normal (ASE).
    vec3 Vtan = normalize(w2t * V);
    float fDot = dot(Vtan, nTan);
    float fres = saturate1(pow(saturate1(1.0 - fDot), P.uFresnelPower));
    vec3 fresnelTerm = (fres * P.uFresnelMult) * P.uFresnelColor.rgb;

    vec3 detailTerm = saturate3(pow(triDetail.rgb, vec3(P.uDetailPow)) * P.uDetailBoost) * P.uDetailTint.rgb;
    vec3 albedo = saturate3(detailTerm + (triAlbedo.rgb * P.uTint.rgb) + fresnelTerm);
    vec3 specColor = P.uSpecular * triSpec.x * P.uSpecularTint.rgb;

    vec3 lit = standardSpecular(albedo, specColor, P.uSmoothness, shadeN, V, L, F.uLightColor, F.uAmbient);

    vec3 scatter = scatterTerm(_ScatterMap, N, L, V, P.uScatterCenterShift, P.uScatterStretch,
                               P.uScatterColor.rgb, F.uLightColor, P.uScatterBoost, P.uScatterIndirect);
    fragColor = vec4(saturate3(scatter * lit), 1.0);
}
