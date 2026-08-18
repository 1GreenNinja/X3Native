#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_gas.frag  — FORGE3D Planets HD / Gas (gas giant), ported to the
//  X3Native bindless pipeline. Animated flow-distorted banded height/albedo
//  (panning lat-long UVs + a distortion map), fresnel rim, specular-workflow
//  PBR (spec from the height tex, smoothness from inverse height), and an
//  atmospheric scatter LUT keyed on (N.L, N.V). ANIMATED (two UV panners).
//
//    distortion = tex(_UVDistortionMap, panUV).r * uUVDistortion
//    bands      = tex(_HeightMap, panUV + distortion)
//    albedo     = fresnel + (uTint * bands.rgb * 2)
//    final      = saturate(scatter * standardSpecular(...))
//
//  NOTE: Gas samples the height/distortion maps with LAT-LONG UV panners
//  (vUV from planet.vert), NOT object-space triplanar — the bands wrap around
//  the equator. uTime (push constant) drives the two panners.
//
//  Helpers (applyST / saturate* / standardSpecular / scatterTerm) are INLINED
//  from planet_common.glsl, with scatterTerm rewritten to sample the engine's
//  BINDLESS array by a uint texture index.
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

// ---- Push constant (model + bindless texture indices + time). Generalized
// 12-slot layout shared by every planet type (supersedes Moon's uvec4+uint).
//   tex[0] = _HeightMap (banded color/albedo+spec, sRGB)
//   tex[1] = _UVDistortionMap (.r flow offset, linear/UNORM)
//   tex[2] = _ScatterMap (sunset_yellow_01 atmosphere LUT, sRGB)
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

// ---- Unity-style ST application: uv * scale + offset (vec4 ST = scale.xy, off.zw)
vec2 applyST(vec2 uv, vec4 st){ return uv * st.xy + st.zw; }

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

// ---- ATMOSPHERIC SCATTER LUT (bindless): coord = (centerShift + (N.L remap,
// N.V)) * stretch; sample the scatter ramp, tint, *boost, +indirect. N = the
// GEOMETRIC world normal. Faithful to the Gas port: raw N.V on Y (can be < 0).
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
//  GAS material constants. Hardcoded in place of the old PlanetParams UBO (P.*),
//  using tasteful values from the FORGE3D Gas ShaderLab defaults. (Params get
//  exposed later; this is a fixed-look first pass.)
// =============================================================================
const vec4  uHeightMap_ST       = vec4(1.0, 1.0, 0.0, 0.0);   // scale.xy, offset.zw
const vec4  uUVDistortionMap_ST = vec4(1.0, 1.0, 0.0, 0.0);
const vec2  uUVSpeed            = vec2(0.008, 0.0);   // slow eastward band drift
const vec2  uUVDistortionSpeed  = vec2(0.004, 0.0);   // distortion crawls slower
const float uUVDistortion       = 0.03;               // flow warp strength
const vec3  uTint               = vec3(1.0, 0.95, 0.85);
const float uSpecular           = 0.06;
const vec3  uSpecularTint       = vec3(1.0);
const float uSmoothness         = 0.30;
const float uFresnelPower       = 4.0;
const float uFresnelMult        = 0.20;
const vec3  uFresnelColor       = vec3(0.55, 0.65, 0.85);
const vec3  uScatterColor       = vec3(1.0, 0.85, 0.6);
const float uScatterBoost       = 1.0;
const float uScatterIndirect    = 0.15;
const float uScatterStretch     = 1.0;   // 1 = identity LUT coord
const float uScatterCenterShift = 0.0;

void main() {
    vec3 N = normalize(vWorldNormal);
    vec3 V = normalize(vViewDir);
    vec3 L = normalize(cam.sunDir.xyz);

    // Light/ambient sourced from the engine Camera UBO. Gas giants are fairly
    // bright (band albedo is near mid-gray); a modest sun radiance reads well.
    vec3 uLightColor = vec3(1.0, 0.96, 0.90);
    vec3 uAmbient    = cam.ambientCount.rgb;

    // ---- Flow distortion -> animated banded albedo (lat-long panners) ----
    vec2 uvDist   = applyST(vUV, uUVDistortionMap_ST);
    vec2 panner11 = uvDist + pc.uTime * uUVDistortionSpeed;
    float distortion = texture(textures[nonuniformEXT(pc.tex[1])], panner11).r * uUVDistortion;

    vec2 uvHeight = applyST(vUV, uHeightMap_ST);
    vec2 panner39 = uvHeight + pc.uTime * uUVSpeed;
    vec4 heightTex = texture(textures[nonuniformEXT(pc.tex[0])], panner39 + vec2(distortion));

    // ---- Fresnel (1 - N.V) ----
    float NoV = saturate1(dot(N, V));
    vec3 fresnel = saturate1(pow(saturate1(1.0 - NoV), uFresnelPower)) * uFresnelMult * uFresnelColor;

    // ---- PBR inputs ----
    vec3  albedo     = fresnel + (uTint * heightTex.rgb * 2.0);
    vec3  specColor  = heightTex.rgb * uSpecular * uSpecularTint;
    float smoothness = (1.0 - heightTex.r) * uSmoothness;
    vec3  lit = standardSpecular(albedo, specColor, smoothness, N, V, L, uLightColor, uAmbient);

    // ---- Scatter (raw N.V on Y, can be negative — faithful) ----
    vec3 scatter = scatterTerm(pc.tex[2], N, L, V, uScatterCenterShift, uScatterStretch,
                               uScatterColor, uLightColor, uScatterBoost, uScatterIndirect);
    fragColor = vec4(saturate3(scatter * lit), 1.0);
}
