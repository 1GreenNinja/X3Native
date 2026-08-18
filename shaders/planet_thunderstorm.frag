#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_thunderstorm.frag  — FORGE3D Planets HD / Thunderstorm (storm world),
//  ported to the X3Native BINDLESS pipeline (mirrors the proven planet_moon.frag
//  structure exactly, generalized to the 12-slot push-constant texture table).
//
//  Animated, flow-distorted cloud NORMAL + animated DETAIL driving a Tint
//  Low<->High gradient, fresnel rim, specular-workflow PBR, atmospheric scatter
//  LUT, and EMISSIVE LIGHTNING:
//    triple clamped-sine flicker * triplanar mask (two scrolling layers) *
//    floor()-quantized triplanar strike (two layers) * lightning color.
//
//    surf  = standardSpecular(fres*FresnelColor + tint, specColor, ...)
//    lit   = saturate(scatter * surf)
//    final = lit + max(0, lightningEmissive)         // ADDITIVE HDR emissive -> bloom
//
//  Output blend class: OPAQUE (A = 1.0). The lightning is added in HDR on top of
//  the lit surface (handled by the scene HDR + ACES tonemap + bloom), so this is
//  a single opaque body draw — NOT a separate additive shell.
//
//  Helpers (triplanar / flowDistortOffset / unpackScaleNormal / saturate* /
//  standardSpecular / scatterTerm) are INLINED from planet_common.glsl, with the
//  texture-sampling helpers rewritten to index the engine's BINDLESS array by a
//  uint texture index (matching planet_moon.frag's bindless triplanar/scatter).
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

// ---- Push constant (model + generalized 12-slot bindless texture table + time).
//  Supersedes Moon's uvec4 layout; the engine is updated to drive planets with
//  this PC for every type. Thunderstorm texture slot mapping:
//    tex[0] = _NormalMap        (Moon/moon_07_normal, RG)        — animated cloud normal
//    tex[1] = _DistortionMap    (Thunderstorm/thunderstorm_0N,R) — detail bands -> Tint gradient
//    tex[2] = _DistortionUVMap  (Terrestrial/terrestrialdetail_04,R) — flow UV distortion
//    tex[3] = _ScatterMap       (Atmosphere/sunset_yellow_05,RGB)— scatter LUT (sRGB)
//    tex[4] = _LightingMaskMap  (Thunderstorm/stormmask,R)       — where lightning is allowed
//    tex[5] = _LightingMap      (Thunderstorm/storm_0N,R)        — lightning strike texture
//    tex[6..11] = unused
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
vec4  saturate4(vec4  x){ return clamp(x, 0.0, 1.0); }

// ---- Tangent-space normal unpack (FORGE3D ships plain RGB normal maps; xy in rg).
vec3 unpackScaleNormal(vec4 packed, float scale){
    vec3 n;
    n.xy = (packed.rg * 2.0 - 1.0) * scale;
    n.z  = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// ---- OBJECT-SPACE TRIPLANAR (bindless): samples textures[texIdx] over the three
// object-axis planes blended by pow(abs(objNormal), falloff). uvOffset carries the
// animated scroll/flow offset. Mirrors planet_common.glsl's triplanar(), rewritten
// to take a uint bindless index instead of a sampler2D.
vec4 triplanar(uint texIdx, vec3 objPos, vec3 objNormal,
               float falloff, float tiling, vec2 uvOffset){
    vec3 w = pow(abs(objNormal), vec3(falloff));
    w /= max(w.x + w.y + w.z, 1e-5);
    vec3 s = sign(objNormal);
    vec2 uvX = objPos.zy * vec2( s.x, 1.0) * tiling + uvOffset;
    vec2 uvY = tiling * objPos.xz * vec2( s.y, 1.0) + uvOffset;
    vec2 uvZ = uvOffset + tiling * objPos.xy * vec2(-s.z, 1.0);
    return w.x * texture(textures[nonuniformEXT(texIdx)], uvX)
         + w.y * texture(textures[nonuniformEXT(texIdx)], uvY)
         + w.z * texture(textures[nonuniformEXT(texIdx)], uvZ);
}
// Convenience: no animation offset.
vec4 triplanar(uint texIdx, vec3 objPos, vec3 objNormal, float falloff, float tiling){
    return triplanar(texIdx, objPos, objNormal, falloff, tiling, vec2(0.0));
}

// ---- FLOW-MAP / UV self-distortion (bindless). Sample a (triplanar) flow texture,
// saturate, scale by `factor`; its RG warps the UV of a second lookup. baseUV =
// scrollOffset + warpRG. (planet_common.glsl flowDistortOffset, bindless variant.)
vec2 flowDistortOffset(uint flowIdx, vec3 objPos, vec3 objNormal,
                       float tiling, float falloff, vec2 flowScroll, float factor){
    vec4 f = clamp(triplanar(flowIdx, objPos, objNormal, falloff, tiling, flowScroll), 0.0, 1.0);
    return f.rg * factor;
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
//  THUNDERSTORM material constants. Hardcoded in place of the old PlanetParams
//  UBO (P.*), taken from the FORGE3D ShaderLab defaults / reference port. This is
//  a fixed-look first pass; params get exposed later.
// =============================================================================
// Animated cloud normal (UV self-distortion + scroll)
const float uNormalUVTiling          = 1.0;
const float uNormalUVSpeed           = 0.02;
const float uNormalTiling            = 1.0;
const float uNormalSpeed             = 0.01;
const float uNormalDistortionFactor  = 0.10;
const float uNormalScale             = 1.0;
// Animated detail -> Tint gradient (UV self-distortion + scroll)
const float uDistortionUVTiling      = 1.0;
const float uDistortionUVSpeed       = 0.015;
const float uDistortionTiling        = 1.0;
const float uDistortionSpeed         = 0.02;
const float uDistortionFactor        = 0.12;
// Tint gradient (storm bands): dark slate-blue base -> bright stormy steel
const vec3  uTintHigh                = vec3(0.55, 0.60, 0.72);
const vec3  uTintLow                 = vec3(0.06, 0.08, 0.14);
const float uDetailPow               = 1.0;
const float uDetailBoost             = 1.0;
// Specular workflow
const vec3  uSpecularColor           = vec3(1.0);
const float uSpecular                = 0.10;
const float uSmoothness              = 0.20;
// Scatter LUT
const vec3  uScatterColor            = vec3(1.0, 0.90, 0.70);
const float uScatterBoost            = 1.0;
const float uScatterIndirect         = 0.20;
const float uScatterStretch          = 1.0;   // 1 = identity LUT coord
const float uScatterCenterShift      = 0.0;
// Fresnel rim (cool storm limb glow)
const float uFresnelMult             = 0.20;
const float uFresnelPower            = 3.0;
const vec3  uFresnelColor            = vec3(0.45, 0.55, 0.85);
// Lightning — flicker masks (two scrolling layers)
const float uLightingMaskATiling     = 1.0;
const float uLightingMaskBTiling     = 1.7;
const float uLightingMaskAUVSpeed    = 0.03;
const float uLightingMaskBUVSpeed    = -0.05;
// Lightning — quantized strike textures (two layers)
const float uLightingATiling         = 1.0;
const float uLightingBTiling         = 1.3;
const float uLightingAFrequency      = 6.0;    // strikes/sec quantization (layer A)
const float uLightingBFrequency      = 9.0;    // strikes/sec quantization (layer B)
const float uLightningSineMult       = 2.0;
const float uLightningMaskPow        = 2.0;
const float uLightningBoost          = 2.0;
const vec3  uLightningColor          = vec3(0.70, 0.80, 1.0) * 6.0;  // hot HDR bolt -> bloom

void main() {
    vec3 oN = normalize(vObjPos);
    vec3 oP = vObjPos;
    vec3 N  = normalize(vWorldNormal);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(cam.sunDir.xyz);

    // Light/ambient from the engine Camera UBO. The storm surface is dark slate
    // cloud (low albedo), so drive it with a >1 sun radiance like Moon did so the
    // lit hemisphere reads brightly (the HDR scene + ACES tonemap absorb the gain).
    vec3 uLightColor = vec3(1.0, 0.96, 0.90) * 3.0;
    vec3 uAmbient    = cam.ambientCount.rgb;

    // ---- Animated normal (triplanar flow-distorted, scrolled) ----
    vec2 nWarp = flowDistortOffset(pc.tex[0], oP, oN, uNormalUVTiling, 5.0,
                                   vec2(pc.uTime * uNormalUVSpeed), uNormalDistortionFactor);
    vec2 nOffset = vec2(pc.uTime * uNormalSpeed) + nWarp;
    vec4 nTri = saturate4(triplanar(pc.tex[0], oP, oN, 5.0, uNormalTiling, nOffset));
    vec3 tN = unpackScaleNormal(nTri, uNormalScale);
    vec3 Npert = normalize(vTBN * tN);

    // ---- Animated detail -> Tint gradient ----
    vec2 dWarp = flowDistortOffset(pc.tex[2], oP, oN, uDistortionUVTiling, 5.0,
                                   vec2(pc.uTime * uDistortionUVSpeed), uDistortionFactor);
    vec2 dOffset = vec2(pc.uTime * uDistortionSpeed) + dWarp;
    vec4 detailTri = saturate4(triplanar(pc.tex[1], oP, oN, 5.0, uDistortionTiling, dOffset));
    vec4 detail = saturate4(pow(detailTri, vec4(uDetailPow)) * uDetailBoost);
    vec3 tint = saturate3(mix(uTintLow, uTintHigh, detail.r));

    float fres = saturate1(pow(saturate1(1.0 - max(dot(V, Npert), 0.0)), uFresnelPower)) * uFresnelMult;
    vec3 albedo = fres * uFresnelColor + tint;
    vec3 specColor = uSpecularColor * detail.rgb * uSpecular;

    vec3 surf = standardSpecular(albedo, specColor, uSmoothness, Npert, V, L, uLightColor, uAmbient);

    // ---- Scatter (geometric normal) ----
    vec3 scatter = scatterTerm(pc.tex[3], N, L, V, uScatterCenterShift, uScatterStretch,
                               uScatterColor, uLightColor, uScatterBoost, uScatterIndirect);
    vec3 lit = saturate3(scatter * surf);

    // ---- Lightning ----
    float s1 = clamp(sin(pc.uTime * (2.0  * PI)), 0.3, 1.0);
    float s2 = clamp(sin(pc.uTime * (5.0  * PI)), 0.5, 1.0);
    float s3 = clamp(sin(pc.uTime * (10.0 * PI)), 0.7, 1.0);
    float sine = s1 * s2 * s3;

    vec4 maskA = saturate4(triplanar(pc.tex[4], oP, oN, 5.0, uLightingMaskATiling, vec2(pc.uTime * uLightingMaskAUVSpeed)));
    vec4 maskB = saturate4(triplanar(pc.tex[4], oP, oN, 5.0, uLightingMaskBTiling, vec2(pc.uTime * uLightingMaskBUVSpeed)));
    vec4 lightningMask = maskA * maskB;
    vec4 maskSine = saturate4(pow(vec4(uLightningSineMult) * vec4(sine) * lightningMask, vec4(uLightningMaskPow)));

    float strikeAOff = floor(pc.uTime * uLightingAFrequency) * 0.9;
    float strikeBOff = 0.9 * floor(pc.uTime * uLightingBFrequency);
    vec4 strikeA = saturate4(triplanar(pc.tex[5], oP, oN, 5.0, uLightingATiling, vec2(strikeAOff)));
    vec4 strikeB = saturate4(triplanar(pc.tex[5], oP, oN, 5.0, uLightingBTiling, vec2(strikeBOff)));
    float lightning = (strikeA * strikeB * uLightningBoost).r;

    vec3 lightningEmissive = max(vec3(0.0), maskSine.rgb * lightning * uLightningColor);

    fragColor = vec4(lit + lightningEmissive, 1.0); // OPAQUE body; HDR emissive -> bloom
}
