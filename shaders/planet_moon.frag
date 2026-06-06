#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_moon.frag  — FORGE3D Planets HD / Moon (airless rocky body), ported to
//  the X3Native bindless pipeline. Object-space triplanar Albedo/Normal/Detail/
//  Specular + detail-boost albedo + fresnel rim + specular-workflow PBR + scatter
//  wrap-light tint. STATIC (no time).
//
//    albedo = saturate(detail + albedo*Tint + fresnel)
//    final  = saturate(scatter * standardSpecular(...))
//
//  Helpers (triplanar / unpackScaleNormal / saturate* / standardSpecular /
//  scatterTerm) are INLINED from planet_common.glsl, with triplanar + scatterTerm
//  rewritten to sample the engine's BINDLESS array by a uint texture index.
// =============================================================================

// ---- Bindless texture array (set0/binding0) — EXACTLY as mesh.frag declares it.
layout(set = 0, binding = 0) uniform sampler2D textures[];

// ---- Per-frame Camera UBO (set1/binding1) — MUST match mesh.frag's block exactly.
struct PointLight { vec4 posRange; vec4 colorPad; };
const int kMaxPointLights = 64;
layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 ambientCount;              // rgb = ambient color, w = active light count
    PointLight lights[kMaxPointLights];
    vec4 camPos;                    // xyz = camera world position
    vec4 sunDir;                    // xyz = direction TOWARD the sun
} cam;

// ---- Push constant (generalized) — model + up to 12 bindless texture indices.
//  Moon texture slot mapping (was uvec4 tex + uint scatterIdx):
//    pc.tex[0] = _Albedo     (RGB, sRGB)   — base color   (triplanar)
//    pc.tex[1] = _Normal     (RG,  linear) — surface normal(triplanar)
//    pc.tex[2] = _DetailMap  (RGB, sRGB)   — detail overlay(triplanar)
//    pc.tex[3] = _SpecularMap(R,   linear) — specular mask (triplanar)
//    pc.tex[4] = _ScatterMap (RGB, sRGB)   — wrap-light/scatter LUT (N.L, N.V)
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

// ---- Tangent-space normal unpack (FORGE3D ships plain RGB normal maps; xy in rg).
vec3 unpackScaleNormal(vec4 packed, float scale){
    vec3 n;
    n.xy = (packed.rg * 2.0 - 1.0) * scale;
    n.z  = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// ---- OBJECT-SPACE TRIPLANAR (bindless): samples textures[texIdx] over the three
// object-axis planes blended by pow(abs(objNormal), falloff). uvOffset is 0 here
// (static Moon). Mirrors the ASE Spherical/Object triplanar in planet_common.glsl.
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

// =============================================================================
//  MOON material constants (airless rocky body). Hardcoded in place of the old
//  PlanetParams UBO (P.*). Sensible defaults per the integration brief.
// =============================================================================
const float uAlbedoTiling   = 1.0;
const float uNormalTiling   = 1.0;
const float uDetailTiling   = 1.0;
const float uSpecularTiling = 1.0;
const vec3  uTint           = vec3(0.78);
const vec3  uDetailTint     = vec3(0.8);
const float uDetailBoost    = 1.0;
const float uDetailPow      = 1.0;
const float uNormalScale    = 1.0;
const float uFresnelPower   = 3.0;
const float uFresnelMult    = 0.15;
const vec3  uFresnelColor   = vec3(0.5, 0.6, 0.8);
const float uSpecular       = 0.08;
const vec3  uSpecularTint   = vec3(1.0);
const float uSmoothness     = 0.12;
const vec3  uScatterColor   = vec3(1.0, 0.85, 0.6);
const float uScatterBoost   = 1.0;
const float uScatterIndirect= 0.2;
const float uScatterStretch = 1.0;   // 0 would collapse the LUT coord to origin; 1 = identity
const float uScatterCenterShift = 0.0;

void main() {
    vec3 oN = normalize(vObjPos);
    vec3 N  = normalize(vWorldNormal);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(cam.sunDir.xyz);
    mat3 w2t = transpose(vTBN);

    // Light/ambient sourced from the engine Camera UBO (was the old PlanetFrame).
    // The Moon's real albedo is very dark rock (~0.04 linear); Unity drives it with a
    // bright directional light. Match that with a >1 sun radiance so the lit
    // hemisphere reads as a bright Moon (the HDR scene + ACES tonemap absorb it).
    vec3 uLightColor = vec3(1.0, 0.96, 0.90) * 4.0;
    vec3 uAmbient    = cam.ambientCount.rgb;

    vec4 triDetail = triplanar(pc.tex[2], vObjPos, oN, 5.0, uDetailTiling);
    vec4 triAlbedo = triplanar(pc.tex[0], vObjPos, oN, 5.0, uAlbedoTiling);
    vec4 triNormal = triplanar(pc.tex[1], vObjPos, oN, 5.0, uNormalTiling);
    vec4 triSpec   = triplanar(pc.tex[3], vObjPos, oN, 5.0, uSpecularTiling);

    vec3 nTan = normalize(unpackScaleNormal(triNormal, uNormalScale));
    vec3 shadeN = normalize(vTBN * nTan);

    // Fresnel uses tangent-space view dir vs tangent normal (ASE).
    vec3 Vtan = normalize(w2t * V);
    float fDot = dot(Vtan, nTan);
    float fres = saturate1(pow(saturate1(1.0 - fDot), uFresnelPower));
    vec3 fresnelTerm = (fres * uFresnelMult) * uFresnelColor;

    vec3 detailTerm = saturate3(pow(triDetail.rgb, vec3(uDetailPow)) * uDetailBoost) * uDetailTint;
    vec3 albedo = saturate3(detailTerm + (triAlbedo.rgb * uTint) + fresnelTerm);
    vec3 specColor = uSpecular * triSpec.x * uSpecularTint;

    vec3 lit = standardSpecular(albedo, specColor, uSmoothness, shadeN, V, L, uLightColor, uAmbient);

    vec3 scatter = scatterTerm(pc.tex[4], N, L, V, uScatterCenterShift, uScatterStretch,
                               uScatterColor, uLightColor, uScatterBoost, uScatterIndirect);
    fragColor = vec4(saturate3(scatter * lit), 1.0);
}
