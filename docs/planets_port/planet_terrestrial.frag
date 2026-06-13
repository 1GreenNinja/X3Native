#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_terrestrial.frag  — FORGE3D Planets HD / Terrestrial (Earth-like)
//  Triplanar biome land (desert/vegetation/mountain) + deep/shallow/shore water
//  selected by a land mask, two-layer animated clouds w/ self-shadow, night-side
//  city lights, atmospheric scatter LUT, land/water fresnel rim.
//
//  final = saturate( scatter * saturate( saturate(clouds*5) + landWater + city ) )
//  Conventions: uSunDir TOWARD light, uTime seconds, object-space triplanar.
// =============================================================================

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec3 vObjPos;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec3 vViewDir;
layout(location = 5) in mat3 vTBN;
layout(location = 0) out vec4 fragColor;

// ---- Globals (shared frame UBO; see ARCHITECTURE.md PlanetFrame) ----
layout(set = 0, binding = 0) uniform PlanetFrame {
    vec3 uSunDir;   float uTime;
    vec3 uCamPos;   float _pf0;
    vec3 uLightColor; float _pf1;
    vec3 uAmbient;    float _pf2;
} F;

// ---- Per-planet params (UBO; one block per planet instance) ----
layout(set = 2, binding = 0) uniform PlanetParams {
    float uGlobalBoost;
    vec4  uFresnelLandColor;  float uFresnelLandPower;  float uFresnelLandMult;
    vec4  uFresnelWaterColor; float uFresnelWaterPower; float uFresnelWaterMult;
    vec4  uCityLightColor;    float uCityLightPopulation;
    vec4  uScatterColor;  float uScatterBoost; float uScatterIndirect; float uScatterStretch; float uScatterCenterShift;
    float uWaterShoreFactor; float uWaterDetailFactor; float uWaterDetailBoost;
    vec4  uWaterShallowColor; vec4 uWaterShoreColor; vec4 uWaterDeepColor; vec4 uWaterSpecularColor;
    float uWaterSpecular; float uWaterSmoothness; float uLandSpecular; float uLandSmoothness;
    float uNormalTiling; float uNormalFresnelScale; float uNormalScale; float uHeightTiling;
    vec4  uBaseColor; vec4 uDesertColor; float uDesertCoverage; float uDesertFactors;
    vec4  uVegetationColor; float uVegetationCoverage; float uVegetationFactors;
    vec4  uMountainColor;   float uMountainCoverage;   float uMountainFactors;
    vec4  uCloudsTint; float uCloudsBlendWeight; float uCloudsTopSpeed; float uCloudsMiddleSpeed;
    float uCloudShadows; float uCloudsBoost;
    vec4  uCloudsTop_ST; vec4 uCloudsMiddle_ST; vec4 uGradient_ST; vec4 uLandMask_ST;
    vec4  uCityLightMaskMap_ST; vec4 uCityLightUVMap_ST;
} P;

// ---- Textures (bindless during integration) ----
layout(set = 3, binding = 0) uniform sampler2D _HeightMap;        // R=base/mtn, G=veg, B=desert/elev
layout(set = 3, binding = 1) uniform sampler2D _LandMask;         // R=land/water, G=shore
layout(set = 3, binding = 2) uniform sampler2D _NormalMap;
layout(set = 3, binding = 3) uniform sampler2D _ScatterMap;
layout(set = 3, binding = 4) uniform sampler2D _Gradient;
layout(set = 3, binding = 5) uniform sampler2D _CloudsTop;
layout(set = 3, binding = 6) uniform sampler2D _CloudsMiddle;
layout(set = 3, binding = 7) uniform sampler2D _CityLightMap;
layout(set = 3, binding = 8) uniform sampler2D _CityLightUVMap;
layout(set = 3, binding = 9) uniform sampler2D _CityLightMaskMap;

vec3 biomeBlend(vec3 prev, vec3 biome, float coverage, float maskTerm, float factors){
    return mix(prev, biome, saturate1((coverage - maskTerm) * factors));
}

void main() {
    vec3 N  = normalize(vWorldNormal);
    vec3 No = normalize(vObjPos);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(F.uSunDir);
    mat3 w2t = transpose(vTBN);

    // ---- Clouds (shared two-layer) ----
    vec3 cloudColor; float cloudShadow;
    cloudsTwoLayer(_CloudsTop, _CloudsMiddle, _Gradient, vUV,
                   P.uCloudsTop_ST, P.uCloudsMiddle_ST, P.uGradient_ST,
                   P.uCloudsTopSpeed, P.uCloudsMiddleSpeed, P.uCloudsBlendWeight,
                   P.uCloudsTint.rgb, P.uCloudsBoost, P.uCloudShadows,
                   L, w2t, F.uTime, cloudColor, cloudShadow);
    vec3 cloudSpec  = max(cloudColor, vec3(0.003));
    vec3 cloudsLit  = standardSpecular(cloudColor, cloudSpec, 0.0, N, V, L, F.uLightColor, F.uAmbient);

    // ---- Triplanar height + land mask ----
    vec4  height = triplanar(_HeightMap, vObjPos, No, 5.0, P.uHeightTiling);
    float hx = height.x, hy = height.y, hz = height.z;
    vec4  landMask = texture(_LandMask, applyST(vUV, P.uLandMask_ST));
    float landSel  = landMask.r;     // 1 -> water selection target
    float shoreRaw = landMask.g;

    // ---- Land biomes ----
    vec3 land = P.uBaseColor.rgb * hx;
    land = saturate3(biomeBlend(land, P.uDesertColor.rgb * hz, P.uDesertCoverage, hx*hz, P.uDesertFactors));
    land = saturate3(biomeBlend(land, P.uVegetationColor.rgb * hy, P.uVegetationCoverage, hx*hz, P.uVegetationFactors));
    land = saturate3(biomeBlend(land, hx * P.uMountainColor.rgb, P.uMountainCoverage, hz, P.uMountainFactors));

    // ---- Water ----
    float waterDepth = saturate1(pow(hx*hz, P.uWaterDetailFactor) * P.uWaterDetailBoost);
    vec3  deepShallow = mix(P.uWaterDeepColor.rgb, P.uWaterShallowColor.rgb, waterDepth);
    float shoreMask   = saturate1(pow(shoreRaw, P.uWaterShoreFactor));
    vec3  waterColor  = mix(P.uWaterShoreColor.rgb + deepShallow, deepShallow, shoreMask);

    vec3  surfaceColor = mix(land, waterColor, landSel);

    // ---- Normals (two scales: lighting + fresnel) ----
    vec4 nTri = triplanar(_NormalMap, vObjPos, No, 5.0, P.uNormalTiling);
    vec3 nLit = unpackScaleNormal(nTri, P.uNormalScale);
    vec3 nFr  = unpackScaleNormal(nTri, P.uNormalFresnelScale);
    vec3 groundN = normalize(vTBN * mix(nLit, vec3(0,0,1), landSel));
    vec3 fresnelN = normalize(vTBN * mix(nFr, nLit, landSel));

    float fres = saturate1(1.0 - dot(V, fresnelN));
    vec3 fresLand  = saturate1(pow(fres, P.uFresnelLandPower))  * P.uFresnelLandMult  * P.uFresnelLandColor.rgb;
    vec3 fresWater = saturate1(pow(fres, P.uFresnelWaterPower)) * P.uFresnelWaterMult * P.uFresnelWaterColor.rgb;
    vec3 fresnelRimColor = mix(fresLand, fresWater, landSel);

    // ---- Ground lighting ----
    vec3  albedoCore = cloudShadow * saturate3(saturate3(P.uGlobalBoost*surfaceColor) + saturate3(F.uLightColor*fresnelRimColor));
    vec3  landSpec  = surfaceColor * P.uLandSpecular;
    vec3  waterSpec = saturate3(vec3(P.uWaterSpecular*(waterDepth+0.1)) * P.uWaterSpecularColor.rgb);
    vec3  groundSpec = mix(landSpec, waterSpec, landSel);
    float landSmooth = P.uLandSmoothness * (hx+hy+hz);
    float groundSmooth = cloudShadow * saturate1(mix(landSmooth, P.uWaterSmoothness, landSel));
    vec3  groundLit = standardSpecular(albedoCore, groundSpec, groundSmooth, groundN, V, L, F.uLightColor, F.uAmbient);

    // ---- City lights (night side) ----
    float cityMask   = texture(_CityLightMaskMap, applyST(vUV, P.uCityLightMaskMap_ST)).r;
    vec4  cityUV     = texture(_CityLightUVMap,   applyST(vUV, P.uCityLightUVMap_ST));
    float cityDetail = texture(_CityLightMap, cityUV.rg).r;
    float nightTerm  = 1.0 - saturate1(dot(N, L));
    float nightGate  = saturate1(nightTerm*nightTerm*nightTerm*nightTerm);
    vec3  cityLights = P.uCityLightColor.rgb
                     * pow(cityMask * cityDetail, 1.0 / max(P.uCityLightPopulation, 1e-5))
                     * (1.0 - landSel) * nightGate;

    // ---- Scatter + composite ----
    vec3 scatter = scatterTerm(_ScatterMap, N, L, V, P.uScatterCenterShift, P.uScatterStretch,
                               P.uScatterColor.rgb, F.uLightColor, P.uScatterBoost, P.uScatterIndirect);
    vec3 inner = saturate3(cloudsLit * 5.0) + groundLit + cityLights;
    fragColor = vec4(saturate3(scatter * saturate3(inner)), 1.0);
}
