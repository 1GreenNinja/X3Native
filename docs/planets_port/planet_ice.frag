#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_ice.frag  — FORGE3D Planets HD / Ice (frozen world)
//  Object-space triplanar Normal/Height/Detail/Color, detail-perturbed height
//  -> Linstep -> Ramp3 (Low/Mid/High ice colors), fresnel rim, specular from
//  detail, subsurface scatter LUT multiplying the lit result. STATIC (no time).
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
    float uColorBoost; float uFresnelMult; float uFresnelPower; vec4 uFresnelColor;
    vec4  uScatterColor; float uScatterBoost; float uScatterIndirect; float uScatterStretch; float uScatterCenterShift;
    float uColorTiling; float uDetailTiling; float uHeightTiling; float uNormalTiling; float uNormalScale;
    float uIceFactorA; float uIceFactorB; float uIceFactorC; float uIceDetail;
    vec4  uRampHigh; vec4 uRampMid; vec4 uRampLow;
    vec4  uSpecularColor; float uSpecular; float uSmoothness;
} P;

layout(set = 3, binding = 0) uniform sampler2D _ColorMap;
layout(set = 3, binding = 1) uniform sampler2D _ScatterMap;
layout(set = 3, binding = 2) uniform sampler2D _DetailMap;   // R,G
layout(set = 3, binding = 3) uniform sampler2D _HeightMap;   // R
layout(set = 3, binding = 4) uniform sampler2D _NormalMap;

void main() {
    vec3 No = normalize(vObjPos);
    vec3 N  = normalize(vWorldNormal);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(F.uSunDir);

    vec4 triNormal = triplanar(_NormalMap, vObjPos, No, 5.0, P.uNormalTiling);
    vec4 triHeight = triplanar(_HeightMap, vObjPos, No, 5.0, P.uHeightTiling);
    vec4 triDetail = triplanar(_DetailMap, vObjPos, No, 5.0, P.uDetailTiling);
    vec4 triColor  = triplanar(_ColorMap,  vObjPos, No, 5.0, P.uColorTiling);

    vec3 bumpTan = unpackScaleNormal(triNormal, P.uNormalScale);
    vec3 shadeN  = normalize(vTBN * bumpTan);

    float detailTexture = pow(triDetail.r * triDetail.g, 0.5);
    float detaledHeight = triHeight.r + ((detailTexture - 0.5) * 2.0 * P.uIceDetail);

    float a = P.uIceFactorA - P.uIceDetail;
    float b = P.uIceDetail  + P.uIceFactorB;
    float heightLinStep = linstep(a, b, detaledHeight);

    vec3 ramp = ramp3(P.uRampLow.rgb, P.uRampMid.rgb, P.uRampHigh.rgb, heightLinStep / max(P.uIceFactorC, 1e-4));
    vec3 detailedDepthColor = saturate3(ramp * triColor.rgb * P.uColorBoost);

    vec3 rim = fresnelRim(V, shadeN, P.uFresnelPower, P.uFresnelMult) * P.uFresnelColor.rgb;
    vec3 albedo = rim + detailedDepthColor;
    vec3 specColor = detailTexture * P.uSpecular * P.uSpecularColor.rgb;

    vec3 lit = standardSpecular(albedo, specColor, P.uSmoothness, shadeN, V, L, F.uLightColor, F.uAmbient);

    // Subsurface scatter LUT (uses GEOMETRIC world normal) multiplies the lit result.
    vec3 scatter = scatterTerm(_ScatterMap, N, L, V, P.uScatterCenterShift, P.uScatterStretch,
                               P.uScatterColor.rgb, F.uLightColor, P.uScatterBoost, P.uScatterIndirect);
    fragColor = vec4(saturate3(scatter * lit), 1.0);
}
