#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_oceanic.frag  — FORGE3D Planets HD / Oceanic (water world), ported to
//  the X3Native bindless pipeline (mirrors the proven planet_moon.frag wiring).
//
//  Depth-tinted water (deep/shallow/shore from a triplanar height map) + two-layer
//  animated clouds w/ self-shadow + fresnel rim + atmospheric scatter LUT.
//  (Same surface family as Terrestrial, minus the land biomes / city lights.)
//
//    final = saturate( scatter * saturate( saturate(clouds*5) + water ) )
//
//  Quirk preserved: _WaterShoreFactor feeds pow(1.0,x)==1, so shoreMask is always
//  1 and _WaterShoreColor has no visible effect (faithful to the shipped graph).
//
//  Helpers (saturate* / unpackScaleNormal / triplanar / scatterTerm /
//  cloudsTwoLayer / standardSpecular / fresnelRim / applyST / rotateUV) are
//  INLINED from planet_common.glsl, with the texture-sampling helpers rewritten
//  to read the engine's BINDLESS array by a uint texture index.
//
//  OPAQUE (blend class: opaque, A = 1.0).
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

// ---- Push constant (generalized 12-slot texture table) — matches planet.vert.
//      pc.tex[i] maps THIS type's textures to bindless indices:
//        tex[0] = _HeightMap    (R,B -> water depth; triplanar)   sRGB
//        tex[1] = _NormalMap    (RG, z reconstructed; triplanar)  linear
//        tex[2] = _ScatterMap   (RGB sunset_green_01 LUT)         sRGB
//        tex[3] = _Gradient     (R polegradient_01 cloud blend)   linear
//        tex[4] = _CloudsTop    (R polar/rotated cloud layer)     sRGB
//        tex[5] = _CloudsMiddle (R belly/scrolled cloud layer)    sRGB
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

const float PI      = 3.14159265358979323846;
const float TWO_PI  = 6.28318530717958647692;

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

// ---- FRESNEL rim. rim = saturate(pow(saturate(1 - dot(V,N)), power)) * mult.
float fresnelRim(vec3 V, vec3 N, float power, float mult){
    float d = dot(normalize(V), normalize(N));
    return clamp(pow(clamp(1.0 - d, 0.0, 1.0), power), 0.0, 1.0) * mult;
}

// ---- ROTATE UV about (0.5,0.5) by angle = speed*time (ASE "Rotator").
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
// object-axis planes blended by pow(abs(objNormal), falloff). Mirrors the ASE
// Spherical/Object triplanar in planet_common.glsl. uvOffset is 0 here (static
// surface; only the clouds animate via their own UV path).
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

// ---- ATMOSPHERIC SCATTER LUT (bindless): coord = (centerShift + (N.L remap, N.V))
// * stretch; sample the scatter ramp, tint, *boost, +indirect. N = geometric normal.
vec3 scatterTerm(uint scatterIdx, vec3 N, vec3 L, vec3 V,
                 float centerShift, float stretch, vec3 color, vec3 light,
                 float boost, float indirect){
    vec2 coord = (vec2(centerShift) + vec2(dot(N, L) * 0.5 + 0.5, dot(N, V))) * stretch;
    vec3 s = saturate3(texture(textures[nonuniformEXT(scatterIdx)], coord).rgb * color * light);
    s = saturate3(s * boost);
    s = saturate3(s + vec3(indirect));
    return s;
}

// ---- CLOUD LAYER (two-layer animated, bindless). Pole layer (CloudsTop): UV
// rotated about center at topSpeed*time. Belly layer (CloudsMiddle): UV scrolled
// in U at midSpeed*time. Blended by gradient ramp ^ blendWeight, plus a self-shadow
// term re-sampling each layer offset by 0.005 toward the light. worldToTangent
// transforms the world light dir into tangent space for the belly shadow offset.
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

// =============================================================================
//  OCEANIC material constants. Hardcoded in place of the old PlanetParams UBO
//  (P.*), values taken from the FORGE3D Oceanic ShaderLab defaults. Fixed-look
//  first pass; params get exposed later.
// =============================================================================
const float uGlobalBoost       = 1.0;
const float uNormalTiling      = 1.0;
const float uNormalScale       = 1.0;
const float uHeightTiling      = 1.0;

const float uFresnelPower      = 3.0;
const float uFresnelMult       = 0.15;
const vec3  uFresnelColor      = vec3(0.5, 0.6, 0.8);

const vec3  uScatterColor      = vec3(0.6, 0.9, 1.0);   // green/teal limb (sunset_green_01)
const float uScatterBoost      = 1.0;
const float uScatterIndirect   = 0.2;
const float uScatterStretch    = 1.0;                   // 1 = identity LUT coord
const float uScatterCenterShift= 0.0;

const float uWaterShoreFactor  = 1.0;                   // feeds pow(1.0,x) quirk (== 1)
const float uWaterDetailFactor = 1.0;
const float uWaterDetailBoost  = 1.0;
const vec3  uWaterShallowColor = vec3(0.06, 0.30, 0.45);
const vec3  uWaterShoreColor   = vec3(0.10, 0.40, 0.45); // no visible effect (quirk)
const vec3  uWaterDeepColor    = vec3(0.01, 0.05, 0.14);
const vec3  uWaterSpecularColor= vec3(1.0);
const float uWaterSpecular     = 0.40;
const float uWaterSmoothness   = 0.90;

const vec3  uCloudsTint        = vec3(1.0);
const float uCloudsBlendWeight = 2.0;
const float uCloudsTopSpeed    = 0.004;
const float uCloudsMiddleSpeed = 0.008;
const float uCloudShadows      = 0.10;
const float uCloudsBoost       = 1.0;
const vec4  uCloudsTop_ST      = vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uCloudsMiddle_ST   = vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uGradient_ST       = vec4(1.0, 1.0, 0.0, 0.0);

void main() {
    vec3 N  = normalize(vWorldNormal);
    vec3 No = normalize(vObjPos);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(cam.sunDir.xyz);
    mat3 w2t = transpose(vTBN);

    // Light/ambient sourced from the engine Camera UBO (was the old PlanetFrame).
    // Oceanic water is fairly dark; give the sun a healthy >1 radiance so the lit
    // hemisphere reads as a bright ocean world under the HDR + ACES tonemap.
    vec3 uLightColor = vec3(1.0, 0.96, 0.90) * 3.0;
    vec3 uAmbient    = cam.ambientCount.rgb;

    // ---- Triplanar height (water depth) + normal ----
    vec4 height = triplanar(pc.tex[0], vObjPos, No, 5.0, uHeightTiling);
    float hx = height.x, hz = height.z;
    vec4 nTri = triplanar(pc.tex[1], vObjPos, No, 5.0, uNormalTiling);
    vec3 nTan = unpackScaleNormal(nTri, uNormalScale);
    vec3 nWorld = normalize(vTBN * nTan);

    float depth = saturate1(pow(hx * hz, uWaterDetailFactor) * uWaterDetailBoost);
    vec3  deepShallow = mix(uWaterDeepColor, uWaterShallowColor, depth);
    float shoreMask = saturate1(pow(1.0, uWaterShoreFactor)); // == 1.0 (faithful quirk)
    vec3  waterColor = mix(uWaterShoreColor + deepShallow, deepShallow, shoreMask);

    // ---- Fresnel (tangent space, as ASE) ----
    vec3 Vtan = normalize(w2t * V);
    vec3 fresnel = fresnelRim(Vtan, nTan, uFresnelPower, uFresnelMult) * uFresnelColor;

    // ---- Clouds ----
    vec3 cloudColor; float cloudShadow;
    cloudsTwoLayer(pc.tex[4], pc.tex[5], pc.tex[3], vUV,
                   uCloudsTop_ST, uCloudsMiddle_ST, uGradient_ST,
                   uCloudsTopSpeed, uCloudsMiddleSpeed, uCloudsBlendWeight,
                   uCloudsTint, uCloudsBoost, uCloudShadows,
                   L, w2t, pc.uTime, cloudColor, cloudShadow);
    vec3 cloudSpec = max(cloudColor, vec3(0.003));
    vec3 cloudsLit = standardSpecular(cloudColor, cloudSpec, 0.0, N, V, L, uLightColor, uAmbient);

    // ---- Water lighting ----
    vec3 waterAlbedo = cloudShadow * saturate3(saturate3(waterColor * uGlobalBoost) + fresnel);
    vec3 waterSpec   = saturate3(uWaterSpecular * (depth + 0.1) * uWaterSpecularColor) * uGlobalBoost;
    float waterSmooth = cloudShadow * uWaterSmoothness;
    vec3 waterLit = standardSpecular(waterAlbedo, waterSpec, waterSmooth, nWorld, V, L, uLightColor, uAmbient);

    // ---- Scatter + composite ----
    vec3 scatter = scatterTerm(pc.tex[2], N, L, V, uScatterCenterShift, uScatterStretch,
                               uScatterColor, uLightColor, uScatterBoost, uScatterIndirect);
    vec3 inner = saturate3(cloudsLit * 5.0) + saturate3(waterLit);
    fragColor = vec4(saturate3(scatter * saturate3(inner)), 1.0);
}
