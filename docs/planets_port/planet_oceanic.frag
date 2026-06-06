#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_oceanic.frag  — FORGE3D Planets HD / Oceanic (water world)
//  Depth-tinted water (deep/shallow/shore from a triplanar height map) + two-layer
//  animated clouds w/ self-shadow + fresnel rim + atmospheric scatter LUT.
//  (Same surface family as Terrestrial, minus the land biomes / city lights.)
//
//  final = saturate( scatter * saturate( saturate(clouds*5) + water ) )
//  Quirk preserved: _WaterShoreFactor feeds pow(1.0,x)==1, so shoreMask is always
//  1 and _WaterShoreColor has no visible effect (faithful to the shipped graph).
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
    float uGlobalBoost;
    float uNormalTiling; float uNormalScale; float uHeightTiling;
    float uFresnelPower; float uFresnelMult; vec4 uFresnelColor;
    vec4  uScatterColor; float uScatterBoost; float uScatterIndirect; float uScatterStretch; float uScatterCenterShift;
    float uWaterShoreFactor; float uWaterDetailFactor; float uWaterDetailBoost;
    vec4  uWaterShallowColor; vec4 uWaterShoreColor; vec4 uWaterDeepColor; vec4 uWaterSpecularColor;
    float uWaterSpecular; float uWaterSmoothness;
    vec4  uCloudsTint; float uCloudsBlendWeight; float uCloudsTopSpeed; float uCloudsMiddleSpeed;
    float uCloudShadows; float uCloudsBoost;
    vec4  uCloudsTop_ST; vec4 uCloudsMiddle_ST; vec4 uGradient_ST;
} P;

layout(set = 3, binding = 0) uniform sampler2D _HeightMap;   // R,B drive water depth
layout(set = 3, binding = 1) uniform sampler2D _NormalMap;
layout(set = 3, binding = 2) uniform sampler2D _ScatterMap;
layout(set = 3, binding = 3) uniform sampler2D _Gradient;
layout(set = 3, binding = 4) uniform sampler2D _CloudsTop;
layout(set = 3, binding = 5) uniform sampler2D _CloudsMiddle;

void main() {
    vec3 N  = normalize(vWorldNormal);
    vec3 No = normalize(vObjPos);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(F.uSunDir);
    mat3 w2t = transpose(vTBN);

    // ---- Triplanar height (water depth) + normal ----
    vec4 height = triplanar(_HeightMap, vObjPos, No, 5.0, P.uHeightTiling);
    float hx = height.x, hz = height.z;
    vec4 nTri = triplanar(_NormalMap, vObjPos, No, 5.0, P.uNormalTiling);
    vec3 nTan = unpackScaleNormal(nTri, P.uNormalScale);
    vec3 nWorld = normalize(vTBN * nTan);

    float depth = saturate1(pow(hx * hz, P.uWaterDetailFactor) * P.uWaterDetailBoost);
    vec3  deepShallow = mix(P.uWaterDeepColor.rgb, P.uWaterShallowColor.rgb, depth);
    float shoreMask = saturate1(pow(1.0, P.uWaterShoreFactor)); // == 1.0 (faithful quirk)
    vec3  waterColor = mix(P.uWaterShoreColor.rgb + deepShallow, deepShallow, shoreMask);

    // ---- Fresnel (tangent space, as ASE) ----
    vec3 Vtan = normalize(w2t * V);
    float dNV = dot(Vtan, normalize(nTan));
    vec3 fresnel = fresnelRim(Vtan, nTan, P.uFresnelPower, P.uFresnelMult) * P.uFresnelColor.rgb;

    // ---- Clouds ----
    vec3 cloudColor; float cloudShadow;
    cloudsTwoLayer(_CloudsTop, _CloudsMiddle, _Gradient, vUV,
                   P.uCloudsTop_ST, P.uCloudsMiddle_ST, P.uGradient_ST,
                   P.uCloudsTopSpeed, P.uCloudsMiddleSpeed, P.uCloudsBlendWeight,
                   P.uCloudsTint.rgb, P.uCloudsBoost, P.uCloudShadows,
                   L, w2t, F.uTime, cloudColor, cloudShadow);
    vec3 cloudSpec = max(cloudColor, vec3(0.003));
    vec3 cloudsLit = standardSpecular(cloudColor, cloudSpec, 0.0, N, V, L, F.uLightColor, F.uAmbient);

    // ---- Water lighting ----
    vec3 waterAlbedo = cloudShadow * saturate3(saturate3(waterColor * P.uGlobalBoost) + fresnel);
    vec3 waterSpec   = saturate3(P.uWaterSpecular * (depth + 0.1) * P.uWaterSpecularColor.rgb) * P.uGlobalBoost;
    float waterSmooth = cloudShadow * P.uWaterSmoothness;
    vec3 waterLit = standardSpecular(waterAlbedo, waterSpec, waterSmooth, nWorld, V, L, F.uLightColor, F.uAmbient);

    // ---- Scatter + composite ----
    vec3 scatter = scatterTerm(_ScatterMap, N, L, V, P.uScatterCenterShift, P.uScatterStretch,
                               P.uScatterColor.rgb, F.uLightColor, P.uScatterBoost, P.uScatterIndirect);
    vec3 inner = saturate3(cloudsLit * 5.0) + saturate3(waterLit);
    fragColor = vec4(saturate3(scatter * saturate3(inner)), 1.0);
}
