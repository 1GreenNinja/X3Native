#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_terrestrial.frag  — FORGE3D Planets HD / Terrestrial (Earth-like),
//  ported to the X3Native BINDLESS pipeline (matches planet_moon.frag).
//
//  Object-space triplanar biome land (base / desert / vegetation / mountain)
//  + deep/shallow/shore water selected by an equirect land mask, two-layer
//  animated clouds with self-shadow, night-side city lights, atmospheric
//  scatter LUT, and land/water fresnel rim.
//
//    final = saturate( scatter * saturate( saturate(clouds*5) + landWater + city ) )
//
//  Conventions: sunDir is TOWARD the light, pc.uTime in seconds, object-space
//  triplanar on a unit sphere (vObjPos == object normal direction).
//
//  Helpers (saturate* / applyST / unpackScaleNormal / triplanar /
//  standardSpecular / scatterTerm / cloudsTwoLayer / rotateUV) are INLINED from
//  planet_common.glsl, with the texture-sampling helpers rewritten to take a
//  uint bindless index instead of a sampler2D.
// =============================================================================

// ---- Bindless texture array (set0/binding0) — EXACTLY as mesh.frag declares it.
layout(set = 0, binding = 0) uniform sampler2D textures[];

// ---- Per-frame Camera UBO (set1/binding1) — MUST match mesh.frag's block exactly.
struct PointLight { vec4 posRange; vec4 colorPad; vec4 dirCone; };
const int kMaxPointLights = 64;
layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 ambientCount;              // rgb = ambient color, w = active light count
    PointLight lights[kMaxPointLights];
    vec4 camPos;                    // xyz = camera world position
    vec4 sunDir;                    // xyz = direction TOWARD the sun
} cam;

// ---- Push constant (model + bindless texture indices + time) — generalized.
//  Texture slot mapping for Terrestrial:
//    tex[0] = _HeightMap        (R=base/mtn, G=veg, B=desert/elev)  sRGB
//    tex[1] = _LandMask         (R=land/water split, G=shore)        linear
//    tex[2] = _NormalMap        (RG, z reconstructed)                linear
//    tex[3] = _ScatterMap       (Atmosphere/sunset ramp)             sRGB
//    tex[4] = _Gradient         (cloud pole<->belly blend, R)        linear
//    tex[5] = _CloudsTop        (polar/rotated cloud layer, R)       sRGB
//    tex[6] = _CloudsMiddle     (belly/scrolled cloud layer, R)      sRGB
//    tex[7] = _CityLightMap     (night city-light detail, R)         linear
//    tex[8] = _CityLightUVMap   (UV remap for city lights, RG)       linear
//    tex[9] = _CityLightMaskMap (population/coverage mask, R)        linear
layout(push_constant) uniform PC {
    mat4  model;
    uint  tex[12];
    float uTime;
    float _p0;
    uint  _p1;
    uint  _p2;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec3 vObjPos;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec3 vViewDir;
layout(location = 5) in mat3 vTBN;
layout(location = 0) out vec4 fragColor;

const float PI = 3.14159265358979323846;

// ---- saturate helpers ------------------------------------------------------
float saturate1(float x){ return clamp(x, 0.0, 1.0); }
vec3  saturate3(vec3  x){ return clamp(x, 0.0, 1.0); }

// Unity-style ST application: uv * scale + offset  (vec4 ST = scale.xy, offset.zw)
vec2 applyST(vec2 uv, vec4 st){ return uv * st.xy + st.zw; }

// ---- Tangent-space normal unpack (FORGE3D ships plain RGB normal maps; xy in rg).
vec3 unpackScaleNormal(vec4 packed, float scale){
    vec3 n;
    n.xy = (packed.rg * 2.0 - 1.0) * scale;
    n.z  = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// ---- ROTATE UV about (0.5,0.5) by angle = speed*time (ASE "Rotator"/"rotateUV").
vec2 rotateUV(vec2 uv, float speed, float time){
    uv -= 0.5;
    float s = sin(speed * time);
    float c = cos(speed * time);
    vec2 r;
    r.x = uv.x * c + uv.y * s;
    r.y = uv.x * (-s) + uv.y * c;
    return r + 0.5;
}

// ---- OBJECT-SPACE TRIPLANAR (bindless): samples textures[texIdx] over the three
// object-axis planes blended by pow(abs(objNormal), falloff). uvOffset is 0 here.
vec4 triplanar(uint texIdx, vec3 objPos, vec3 objNormal, float falloff, float tiling){
    vec3 w = pow(abs(objNormal), vec3(falloff));
    w /= max(w.x + w.y + w.z, 1e-5);
    vec3 s = sign(objNormal);
    vec2 uvX = objPos.zy * vec2( s.x, 1.0) * tiling;
    vec2 uvY = tiling * objPos.xz * vec2( s.y, 1.0);
    vec2 uvZ = tiling * objPos.xy * vec2(-s.z, 1.0);
    return w.x * texture(textures[nonuniformEXT(texIdx)], uvX)
         + w.y * texture(textures[nonuniformEXT(texIdx)], uvY)
         + w.z * texture(textures[nonuniformEXT(texIdx)], uvZ);
}

// ---- LIGHTING — compact Unity LightingStandardSpecular stand-in (single sun).
float D_GGX(float NoH, float a){
    float a2 = a * a;
    float d  = (NoH * NoH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}
float V_SmithGGX(float NoV, float NoL, float a){
    float k = (a * a) * 0.5;
    float gv = NoV / (NoV * (1.0 - k) + k);
    float gl = NoL / (NoL * (1.0 - k) + k);
    return gv * gl;
}
vec3 F_Schlick(vec3 f0, float u){ return f0 + (vec3(1.0) - f0) * pow(1.0 - u, 5.0); }

vec3 standardSpecular(vec3 albedo, vec3 specColor, float smoothness,
                      vec3 N, vec3 V, vec3 L, vec3 lightColor, vec3 ambient){
    float rough = max(1.0 - clamp(smoothness, 0.0, 1.0), 0.045);
    vec3  H   = normalize(L + V);
    float NoL = max(dot(N, L), 0.0);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);
    float oneMinusRefl = 1.0 - max(max(specColor.r, specColor.g), specColor.b);
    vec3  diffuse  = albedo * oneMinusRefl * NoL;
    vec3  specular = (D_GGX(NoH, rough) * V_SmithGGX(NoV, NoL, rough)) * F_Schlick(specColor, VoH) * NoL;
    return (diffuse + specular) * lightColor + albedo * ambient;
}

// ---- ATMOSPHERIC SCATTER LUT (bindless). N = geometric world normal.
vec3 scatterTerm(uint scatterIdx, vec3 N, vec3 L, vec3 V,
                 float centerShift, float stretch, vec3 color, vec3 light,
                 float boost, float indirect){
    vec2 coord = (vec2(centerShift) + vec2(dot(N, L) * 0.5 + 0.5, dot(N, V))) * stretch;
    vec3 s = saturate3(texture(textures[nonuniformEXT(scatterIdx)], coord).rgb * color * light);
    s = saturate3(s * boost);
    s = saturate3(s + vec3(indirect));
    return s;
}

// ---- CLOUD LAYER (two-layer animated), bindless. Pole layer rotated, belly layer
// scrolled in U, blended by a gradient ramp ^ blendWeight, plus a self-shadow term.
void cloudsTwoLayer(
    uint cloudsTop, uint cloudsMiddle, uint gradient,
    vec2 uv, vec4 topST, vec4 midST, vec4 gradST,
    float topSpeed, float midSpeed, float blendWeight,
    vec3 cloudsTint, float cloudsBoost, float cloudShadows,
    vec3 worldLightDir, mat3 worldToTangent, float time,
    out vec3 cloudColor, out float cloudShadow)
{
    vec2 uvTop  = applyST(uv, topST);
    vec2 poleUV = rotateUV(uvTop, topSpeed, time);
    float cloudPole = texture(textures[nonuniformEXT(cloudsTop)], poleUV).r;

    vec2 uvMid   = applyST(uv, midST);
    vec2 bellyUV = vec2(uvMid.x + midSpeed * time, uvMid.y);
    float cloudBelly = texture(textures[nonuniformEXT(cloudsMiddle)], bellyUV).r;

    vec2 uvGrad = applyST(uv, gradST);
    float gradientMap = pow(texture(textures[nonuniformEXT(gradient)], uvGrad).r, blendWeight);

    float cloudMix = mix(cloudPole, cloudBelly, gradientMap);
    cloudColor = saturate3(vec3(cloudMix) * cloudsTint * cloudsBoost);

    // Self-shadow: offset-sample each layer toward the light, blend, ramp.
    vec2 shadowUVPole = vec2(0.005 * worldLightDir.x);
    float poleShadow  = texture(textures[nonuniformEXT(cloudsTop)], poleUV + shadowUVPole).r;
    vec3  Ltan        = normalize(worldToTangent * worldLightDir);
    vec2  shadowUVBelly = vec2(Ltan.x * 0.005);
    float bellyShadow = texture(textures[nonuniformEXT(cloudsMiddle)], bellyUV + shadowUVBelly).r;
    float mixShadow   = mix(poleShadow, bellyShadow, gradientMap + 0.1);
    cloudShadow = clamp(pow(1.0 - mixShadow, cloudShadows * 50.0), 0.0, 1.0);
}

vec3 biomeBlend(vec3 prev, vec3 biome, float coverage, float maskTerm, float factors){
    return mix(prev, biome, saturate1((coverage - maskTerm) * factors));
}

// =============================================================================
//  TERRESTRIAL material constants. Hardcoded in place of the old PlanetParams
//  UBO (P.*) — tasteful Earth-like defaults (fixed-look first pass).
// =============================================================================
const float uGlobalBoost        = 1.0;

const vec3  uFresnelLandColor   = vec3(0.55, 0.70, 1.00);
const float uFresnelLandPower   = 3.0;
const float uFresnelLandMult    = 0.20;
const vec3  uFresnelWaterColor  = vec3(0.45, 0.65, 1.00);
const float uFresnelWaterPower  = 4.0;
const float uFresnelWaterMult   = 0.45;

const vec3  uCityLightColor     = vec3(1.0, 0.85, 0.55);
const float uCityLightPopulation= 2.5;

const vec3  uScatterColor       = vec3(1.0, 0.92, 0.78);
const float uScatterBoost       = 1.0;
const float uScatterIndirect    = 0.18;
const float uScatterStretch     = 1.0;   // 1 = identity LUT coord
const float uScatterCenterShift = 0.0;

const float uWaterShoreFactor   = 2.0;
const float uWaterDetailFactor  = 1.2;
const float uWaterDetailBoost   = 1.5;
const vec3  uWaterShallowColor  = vec3(0.10, 0.45, 0.55);
const vec3  uWaterShoreColor    = vec3(0.06, 0.22, 0.28);
const vec3  uWaterDeepColor     = vec3(0.02, 0.08, 0.18);
const vec3  uWaterSpecularColor = vec3(1.0, 1.0, 1.0);
const float uWaterSpecular      = 0.30;
const float uWaterSmoothness    = 0.90;
const float uLandSpecular       = 0.04;
const float uLandSmoothness     = 0.15;

const float uNormalTiling       = 1.0;
const float uNormalFresnelScale = 0.30;
const float uNormalScale        = 1.0;
const float uHeightTiling       = 1.0;

const vec3  uBaseColor          = vec3(0.30, 0.26, 0.18);
const vec3  uDesertColor        = vec3(0.72, 0.60, 0.36);
const float uDesertCoverage     = 0.55;
const float uDesertFactors      = 3.0;
const vec3  uVegetationColor    = vec3(0.18, 0.34, 0.12);
const float uVegetationCoverage = 0.60;
const float uVegetationFactors  = 3.0;
const vec3  uMountainColor      = vec3(0.42, 0.38, 0.34);
const float uMountainCoverage   = 0.55;
const float uMountainFactors    = 3.0;

const vec3  uCloudsTint         = vec3(1.0, 1.0, 1.0);
const float uCloudsBlendWeight  = 2.0;
const float uCloudsTopSpeed     = 0.004;
const float uCloudsMiddleSpeed  = 0.006;
const float uCloudShadows       = 0.30;
const float uCloudsBoost        = 1.0;

const vec4  uCloudsTop_ST       = vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uCloudsMiddle_ST    = vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uGradient_ST        = vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uLandMask_ST        = vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uCityLightMaskMap_ST= vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uCityLightUVMap_ST  = vec4(1.0, 1.0, 0.0, 0.0);

void main() {
    vec3 N  = normalize(vWorldNormal);
    vec3 No = normalize(vObjPos);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(cam.sunDir.xyz);
    mat3 w2t = transpose(vTBN);

    // Light/ambient sourced from the engine Camera UBO (was the old PlanetFrame).
    // Terrestrial albedo is mid-bright; a modest >1 sun keeps the lit hemisphere
    // reading well under the HDR + ACES tonemap (less boost than the dark Moon).
    vec3 uLightColor = vec3(1.0, 0.96, 0.90) * 2.0;
    vec3 uAmbient    = cam.ambientCount.rgb;

    // ---- Clouds (shared two-layer) ----
    vec3 cloudColor; float cloudShadow;
    cloudsTwoLayer(pc.tex[5], pc.tex[6], pc.tex[4], vUV,
                   uCloudsTop_ST, uCloudsMiddle_ST, uGradient_ST,
                   uCloudsTopSpeed, uCloudsMiddleSpeed, uCloudsBlendWeight,
                   uCloudsTint, uCloudsBoost, uCloudShadows,
                   L, w2t, pc.uTime, cloudColor, cloudShadow);
    vec3 cloudSpec  = max(cloudColor, vec3(0.003));
    vec3 cloudsLit  = standardSpecular(cloudColor, cloudSpec, 0.0, N, V, L, uLightColor, uAmbient);

    // ---- Triplanar height + land mask ----
    vec4  height = triplanar(pc.tex[0], vObjPos, No, 5.0, uHeightTiling);
    float hx = height.x, hy = height.y, hz = height.z;
    vec4  landMask = texture(textures[nonuniformEXT(pc.tex[1])], applyST(vUV, uLandMask_ST));
    float landSel  = landMask.r;     // 1 -> water selection target
    float shoreRaw = landMask.g;

    // ---- Land biomes ----
    vec3 land = uBaseColor * hx;
    land = saturate3(biomeBlend(land, uDesertColor * hz, uDesertCoverage, hx*hz, uDesertFactors));
    land = saturate3(biomeBlend(land, uVegetationColor * hy, uVegetationCoverage, hx*hz, uVegetationFactors));
    land = saturate3(biomeBlend(land, hx * uMountainColor, uMountainCoverage, hz, uMountainFactors));

    // ---- Water ----
    float waterDepth = saturate1(pow(hx*hz, uWaterDetailFactor) * uWaterDetailBoost);
    vec3  deepShallow = mix(uWaterDeepColor, uWaterShallowColor, waterDepth);
    float shoreMask   = saturate1(pow(shoreRaw, uWaterShoreFactor));
    vec3  waterColor  = mix(uWaterShoreColor + deepShallow, deepShallow, shoreMask);

    vec3  surfaceColor = mix(land, waterColor, landSel);

    // ---- Normals (two scales: lighting + fresnel) ----
    vec4 nTri = triplanar(pc.tex[2], vObjPos, No, 5.0, uNormalTiling);
    vec3 nLit = unpackScaleNormal(nTri, uNormalScale);
    vec3 nFr  = unpackScaleNormal(nTri, uNormalFresnelScale);
    vec3 groundN  = normalize(vTBN * mix(nLit, vec3(0,0,1), landSel));
    vec3 fresnelN = normalize(vTBN * mix(nFr, nLit, landSel));

    float fres = saturate1(1.0 - dot(V, fresnelN));
    vec3 fresLand  = saturate1(pow(fres, uFresnelLandPower))  * uFresnelLandMult  * uFresnelLandColor;
    vec3 fresWater = saturate1(pow(fres, uFresnelWaterPower)) * uFresnelWaterMult * uFresnelWaterColor;
    vec3 fresnelRimColor = mix(fresLand, fresWater, landSel);

    // ---- Ground lighting ----
    vec3  albedoCore = cloudShadow * saturate3(saturate3(uGlobalBoost*surfaceColor) + saturate3(uLightColor*fresnelRimColor));
    vec3  landSpec  = surfaceColor * uLandSpecular;
    vec3  waterSpec = saturate3(vec3(uWaterSpecular*(waterDepth+0.1)) * uWaterSpecularColor);
    vec3  groundSpec = mix(landSpec, waterSpec, landSel);
    float landSmooth = uLandSmoothness * (hx+hy+hz);
    float groundSmooth = cloudShadow * saturate1(mix(landSmooth, uWaterSmoothness, landSel));
    vec3  groundLit = standardSpecular(albedoCore, groundSpec, groundSmooth, groundN, V, L, uLightColor, uAmbient);

    // ---- City lights (night side) ----
    float cityMask   = texture(textures[nonuniformEXT(pc.tex[9])], applyST(vUV, uCityLightMaskMap_ST)).r;
    vec4  cityUV     = texture(textures[nonuniformEXT(pc.tex[8])], applyST(vUV, uCityLightUVMap_ST));
    float cityDetail = texture(textures[nonuniformEXT(pc.tex[7])], cityUV.rg).r;
    float nightTerm  = 1.0 - saturate1(dot(N, L));
    float nightGate  = saturate1(nightTerm*nightTerm*nightTerm*nightTerm);
    vec3  cityLights = uCityLightColor
                     * pow(cityMask * cityDetail, 1.0 / max(uCityLightPopulation, 1e-5))
                     * (1.0 - landSel) * nightGate;

    // ---- Scatter + composite ----
    vec3 scatter = scatterTerm(pc.tex[3], N, L, V, uScatterCenterShift, uScatterStretch,
                               uScatterColor, uLightColor, uScatterBoost, uScatterIndirect);
    vec3 inner = saturate3(cloudsLit * 5.0) + groundLit + cityLights;
    fragColor = vec4(saturate3(scatter * saturate3(inner)), 1.0);
}
