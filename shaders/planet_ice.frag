#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_ice.frag  — FORGE3D Planets HD / Ice (frozen world), ported to the
//  X3Native BINDLESS pipeline. Object-space triplanar Normal/Height/Detail/Color,
//  detail-perturbed height -> Linstep -> Ramp3 (Low/Mid/High ice colors), fresnel
//  rim, specular driven by the detail mask, subsurface-scatter LUT MULTIPLYING the
//  lit result. STATIC (no time).
//
//    detailTexture = sqrt(detail.r * detail.g)
//    height'       = height.r + (detailTexture-0.5)*2*iceDetail
//    t             = linstep(factorA-iceDetail, iceDetail+factorB, height')
//    color         = saturate( ramp3(low,mid,high, t/factorC) * colorMap * boost )
//    albedo        = color + fresnelRim
//    final         = saturate( scatter * standardSpecular(...) )
//
//  Helpers (saturate*/unpackScaleNormal/linstep/ramp3/fresnelRim/triplanar/
//  standardSpecular/scatterTerm) are INLINED from planet_common.glsl, with
//  triplanar + scatterTerm rewritten to sample the engine's BINDLESS array by a
//  uint texture index (mirrors planet_moon.frag).
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

// ---- Push constant (generalized) — model + up to 12 bindless texture indices.
//  Ice texture slot mapping:
//    pc.tex[0] = ColorMap   (RGB, sRGB)   — ice color tint   (triplanar)
//    pc.tex[1] = NormalMap  (RG,  linear) — surface normal   (triplanar)
//    pc.tex[2] = HeightMap  (R,   linear) — elevation        (triplanar)
//    pc.tex[3] = DetailMap  (R,G, linear) — fine detail/spec (triplanar)
//    pc.tex[4] = ScatterMap (RGB, sRGB)   — subsurface scatter LUT (N.L, N.V)
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

// ---- ASE "Linstep": saturate((x-a)/(b-a)).
float linstep(float a, float b, float x){ return clamp((x - a) / (b - a), 0.0, 1.0); }

// ---- ASE "Ramp3": two-stage clamped gradient. t is pre-divided by the scale.
vec3 ramp3(vec3 low, vec3 mid, vec3 high, float t){
    vec3 c = mix(low, mid,  clamp(t,       0.0, 1.0));
    return  mix(c,   high, clamp(t - 1.0, 0.0, 1.0));
}

// ---- FRESNEL rim. rim = saturate(pow(saturate(1 - dot(V,N)), power)) * mult.
float fresnelRim(vec3 V, vec3 N, float power, float mult){
    float d = dot(normalize(V), normalize(N));
    return clamp(pow(clamp(1.0 - d, 0.0, 1.0), power), 0.0, 1.0) * mult;
}

// ---- Tangent-space normal unpack (FORGE3D ships plain RGB normal maps; xy in rg).
vec3 unpackScaleNormal(vec4 packed, float scale){
    vec3 n;
    n.xy = (packed.rg * 2.0 - 1.0) * scale;
    n.z  = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// ---- OBJECT-SPACE TRIPLANAR (bindless): samples textures[texIdx] over the three
// object-axis planes blended by pow(abs(objNormal), falloff). Static (uvOffset=0).
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

// ---- SUBSURFACE-SCATTER LUT (bindless): coord = (centerShift + (N.L remap, N.V))
// * stretch; sample ramp, tint, *boost, +indirect. N = GEOMETRIC world normal.
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
//  ICE material constants (frozen world). Hardcoded in place of the old
//  PlanetParams UBO (P.*). Tasteful fixed-look defaults per the reference port's
//  ShaderLab values — a cold blue/white ramp with a cyan fresnel limb.
// =============================================================================
const float uColorTiling   = 1.0;
const float uDetailTiling  = 1.0;
const float uHeightTiling  = 1.0;
const float uNormalTiling  = 1.0;
const float uNormalScale   = 1.0;
const float uColorBoost    = 1.0;

// Detail-perturbed height -> Linstep -> Ramp3 controls.
const float uIceDetail     = 0.15;   // how much fine detail nudges the height
const float uIceFactorA    = 0.40;   // linstep low edge base
const float uIceFactorB    = 0.45;   // linstep high edge base
const float uIceFactorC    = 1.50;   // ramp spread (t /= factorC)

// Ramp3 ice colors (deep ice -> snow). Low=bluish ice, Mid=pale ice, High=snow.
const vec3  uRampLow        = vec3(0.40, 0.55, 0.75);
const vec3  uRampMid        = vec3(0.72, 0.83, 0.92);
const vec3  uRampHigh       = vec3(0.96, 0.98, 1.00);

// Fresnel limb glow (cold cyan).
const float uFresnelPower   = 4.0;
const float uFresnelMult    = 0.25;
const vec3  uFresnelColor   = vec3(0.55, 0.78, 1.00);

// Specular (ice is glossy; spec driven by the detail mask).
const float uSpecular       = 0.18;
const vec3  uSpecularColor   = vec3(1.0);
const float uSmoothness     = 0.70;

// Subsurface scatter LUT (sunset_yellow_05 ramp; cool tint for an icy limb).
const vec3  uScatterColor   = vec3(0.80, 0.90, 1.00);
const float uScatterBoost   = 1.0;
const float uScatterIndirect= 0.25;
const float uScatterStretch = 1.0;   // 1 = identity coord
const float uScatterCenterShift = 0.0;

void main() {
    vec3 No = normalize(vObjPos);
    vec3 N  = normalize(vWorldNormal);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(cam.sunDir.xyz);

    // Light/ambient sourced from the engine Camera UBO. Ice albedo is bright
    // (snow ~0.9), so the sun radiance stays near 1 (unlike Moon's 4x boost).
    vec3 uLightColor = vec3(1.0, 0.96, 0.90);
    vec3 uAmbient    = cam.ambientCount.rgb;

    vec4 triNormal = triplanar(pc.tex[1], vObjPos, No, 5.0, uNormalTiling);
    vec4 triHeight = triplanar(pc.tex[2], vObjPos, No, 5.0, uHeightTiling);
    vec4 triDetail = triplanar(pc.tex[3], vObjPos, No, 5.0, uDetailTiling);
    vec4 triColor  = triplanar(pc.tex[0], vObjPos, No, 5.0, uColorTiling);

    vec3 bumpTan = unpackScaleNormal(triNormal, uNormalScale);
    vec3 shadeN  = normalize(vTBN * bumpTan);

    float detailTexture = pow(triDetail.r * triDetail.g, 0.5);
    float detaledHeight = triHeight.r + ((detailTexture - 0.5) * 2.0 * uIceDetail);

    float a = uIceFactorA - uIceDetail;
    float b = uIceDetail  + uIceFactorB;
    float heightLinStep = linstep(a, b, detaledHeight);

    vec3 ramp = ramp3(uRampLow, uRampMid, uRampHigh, heightLinStep / max(uIceFactorC, 1e-4));
    vec3 detailedDepthColor = saturate3(ramp * triColor.rgb * uColorBoost);

    vec3 rim = fresnelRim(V, shadeN, uFresnelPower, uFresnelMult) * uFresnelColor;
    vec3 albedo = rim + detailedDepthColor;
    vec3 specColor = detailTexture * uSpecular * uSpecularColor;

    vec3 lit = standardSpecular(albedo, specColor, uSmoothness, shadeN, V, L, uLightColor, uAmbient);

    // Subsurface scatter LUT (uses GEOMETRIC world normal) multiplies the lit result.
    vec3 scatter = scatterTerm(pc.tex[4], N, L, V, uScatterCenterShift, uScatterStretch,
                               uScatterColor, uLightColor, uScatterBoost, uScatterIndirect);
    fragColor = vec4(saturate3(scatter * lit), 1.0);
}
