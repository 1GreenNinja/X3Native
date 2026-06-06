#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_sand.frag  — FORGE3D Planets HD / Sandstorm (desert/dust world)
//  Triplanar detail+normal -> base/desert/mountain color blends, two-layer
//  animated clouds (dust) w/ self-shadow, atmospheric scatter LUT, fresnel rim.
//
//  final = saturate( scatter * saturate( saturate(clouds*CloudBoost) + ground ) )
//  _DetailMap channels: R=base/spec mask, G=mountain mask, B=desert mask.
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
    vec4  uScatterColor; float uScatterBoost; float uScatterIndirect; float uScatterStretch; float uScatterCenterShift;
    float uNormalScale; float uNormalTiling; float uDetailTiling;
    vec4  uSpecularColor; float uSpecular; float uSmoothness;
    float uCloudBoost;
    vec4  uBaseColor; vec4 uDesertColor; float uDesertCoverage; float uDesertFactors; float uAlbedoBoost;
    vec4  uMountainColor; float uMountainCoverage; float uMountainFactors;
    float uFresnelMult; float uFresnelPower; vec4 uFresnelColor;
    vec4  uCloudsTop_ST; vec4 uCloudsMiddle_ST; vec4 uGradient_ST;
    float uCloudsBlendWeight; float uCloudsTopSpeed; float uCloudsMiddleSpeed;
    vec4  uCloudsTint; float uCloudShadows; float uCloudsBoost;
} P;

layout(set = 3, binding = 0) uniform sampler2D _ScatterMap;
layout(set = 3, binding = 1) uniform sampler2D _DetailMap;   // R=base, G=mtn, B=desert
layout(set = 3, binding = 2) uniform sampler2D _NormalMap;
layout(set = 3, binding = 3) uniform sampler2D _Gradient;
layout(set = 3, binding = 4) uniform sampler2D _CloudsTop;
layout(set = 3, binding = 5) uniform sampler2D _CloudsMiddle;

void main() {
    vec3 N  = normalize(vWorldNormal);
    vec3 No = normalize(vObjPos);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(F.uSunDir);
    mat3 w2t = transpose(vTBN);

    // ---- Clouds ----
    vec3 cloudColor; float cloudShadow;
    cloudsTwoLayer(_CloudsTop, _CloudsMiddle, _Gradient, vUV,
                   P.uCloudsTop_ST, P.uCloudsMiddle_ST, P.uGradient_ST,
                   P.uCloudsTopSpeed, P.uCloudsMiddleSpeed, P.uCloudsBlendWeight,
                   P.uCloudsTint.rgb, P.uCloudsBoost, P.uCloudShadows,
                   L, w2t, F.uTime, cloudColor, cloudShadow);
    vec3 cloudSpec = max(cloudColor, vec3(0.003));
    vec3 cloudsLit = standardSpecular(cloudColor, cloudSpec, 0.0, N, V, L, F.uLightColor, F.uAmbient);

    // ---- Ground (triplanar detail + color blends) ----
    vec4 detail = triplanar(_DetailMap, No, No, 5.0, P.uDetailTiling);
    float dX = detail.x, dY = detail.y, dZ = detail.z;

    float desertT = saturate1((P.uDesertCoverage - dX*dZ) * P.uDesertFactors);
    vec3 desertResult = mix(P.uBaseColor.rgb * dX, P.uDesertColor.rgb * dZ, desertT);
    float mountT = saturate1((P.uMountainCoverage - dY) * P.uMountainFactors);
    vec3 terrain = saturate3(mix(saturate3(desertResult), dX * P.uMountainColor.rgb, mountT));

    vec4 nTri = triplanar(_NormalMap, No, No, 5.0, P.uNormalTiling);
    vec3 nTan = normalize(unpackScaleNormal(nTri, P.uNormalScale));
    vec3 groundN = normalize(vTBN * nTan);

    float fdot = dot(V, groundN);
    vec3 fresnelColor = saturate1(pow(saturate1(1.0 - fdot), P.uFresnelPower)) * P.uFresnelMult * P.uFresnelColor.rgb;

    vec3 albedoCore  = saturate3(saturate3(terrain*terrain*P.uAlbedoBoost) + fresnelColor);
    vec3 groundAlbedo = cloudShadow * albedoCore;
    vec3 groundSpec  = dX * P.uSpecular * P.uSpecularColor.rgb;
    float groundSmooth = P.uSmoothness * cloudShadow;
    vec3 groundLit = standardSpecular(groundAlbedo, groundSpec, groundSmooth, groundN, V, L, F.uLightColor, F.uAmbient);

    // ---- Scatter + composite ----
    vec3 scatter = scatterTerm(_ScatterMap, N, L, V, P.uScatterCenterShift, P.uScatterStretch,
                               P.uScatterColor.rgb, F.uLightColor, P.uScatterBoost, P.uScatterIndirect);
    vec3 lit = saturate3(saturate3(cloudsLit * P.uCloudBoost) + groundLit);
    fragColor = vec4(saturate3(scatter * lit), 1.0);
}
