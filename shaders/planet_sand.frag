#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_sand.frag  — FORGE3D Planets HD / Sandstorm (desert/dust world), ported
//  to the X3Native bindless pipeline. OBJECT-space triplanar Detail(+Normal) ->
//  base/desert/mountain color blends, two-layer ANIMATED dust clouds (polar cap +
//  scrolled belly) with self-shadow, atmospheric scatter LUT, and a fresnel rim.
//
//    ground = saturate( saturate(terrain^2 * AlbedoBoost) + fresnel ) * cloudShadow
//    lit    = saturate( saturate(cloudsLit * CloudBoost) + groundLit )
//    final  = saturate( scatter * lit )
//
//  _DetailMap channels:  R = base/spec mask,  G = mountain mask,  B = desert mask.
//
//  Helpers (triplanar / unpackScaleNormal / saturate* / applyST / rotateUV /
//  cloudsTwoLayer / standardSpecular / scatterTerm) are INLINED from
//  planet_common.glsl, with triplanar / cloudsTwoLayer / scatterTerm rewritten to
//  sample the engine's BINDLESS array by a uint texture index. uTime drives the
//  cloud flow. STRUCTURE mirrors the proven planet_moon.frag port. BLEND: opaque.
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

// ---- Push constant (generalized: model + up to 12 bindless texture indices + time).
//  Sand texture-slot mapping (document):
//      tex[0] = _DetailMap   (R=base/spec, G=mtn, B=desert)  — triplanar control
//      tex[1] = _NormalMap   (RG, z reconstructed)           — triplanar normal
//      tex[2] = _ScatterMap  (Atmosphere/sunset_yellow_*)     — scatter LUT
//      tex[3] = _Gradient    (Misc/polegradient_01, R)        — dust-cloud blend
//      tex[4] = _CloudsTop   (dustcloudcap_*, R)              — polar (rotated) cap
//      tex[5] = _CloudsMiddle(dustcloud_*, R)                 — belly (scrolled) band
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

// ---- Unity-style ST application: uv * scale + offset (vec4 ST = scale.xy, offset.zw)
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
// Mirrors the ASE Spherical/Object triplanar in planet_common.glsl.
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

// ---- TWO-LAYER ANIMATED CLOUD LAYER (bindless) — polar cap (rotated) + belly band
// (scrolled in U), blended by a pole<->belly gradient ^ blendWeight, plus a
// re-sampled self-shadow term offset toward the light. Mirrors cloudsTwoLayer in
// planet_common.glsl with the three texture args turned into bindless uint indices.
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
//  SANDSTORM material constants (desert/dust world). Hardcoded in place of the old
//  PlanetParams UBO (P.*). Values taken from the FORGE3D ShaderLab defaults / the
//  reference port; tasteful first-pass fixed look (params get exposed later).
// =============================================================================
const float uDetailTiling   = 1.0;
const float uNormalTiling   = 1.0;
const float uNormalScale    = 1.0;

// Terrain biome colors + coverage controls (R=base/spec, G=mtn, B=desert detail).
const vec3  uBaseColor      = vec3(0.62, 0.45, 0.28);   // dry ochre rock
const vec3  uDesertColor    = vec3(0.84, 0.68, 0.42);   // warm sand dunes
const vec3  uMountainColor  = vec3(0.50, 0.34, 0.22);   // darker mountain rock
const float uDesertCoverage = 0.55;
const float uDesertFactors  = 3.0;
const float uMountainCoverage = 0.45;
const float uMountainFactors  = 3.0;
const float uAlbedoBoost    = 2.0;   // terrain^2 * boost (squared darkens; boost recovers)

// Specular (specular workflow F0).
const float uSpecular       = 0.06;
const vec3  uSpecularColor   = vec3(1.0);
const float uSmoothness      = 0.10;

// Fresnel rim (warm dusty haze at the limb).
const float uFresnelPower   = 3.0;
const float uFresnelMult    = 0.20;
const vec3  uFresnelColor    = vec3(0.85, 0.70, 0.45);

// Two-layer dust clouds.
const vec4  uCloudsTop_ST     = vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uCloudsMiddle_ST  = vec4(1.0, 1.0, 0.0, 0.0);
const vec4  uGradient_ST      = vec4(1.0, 1.0, 0.0, 0.0);
const float uCloudsTopSpeed   = 0.004;   // polar cap rotation (rad/s)
const float uCloudsMiddleSpeed= 0.010;   // belly scroll in U (uv/s)
const float uCloudsBlendWeight= 2.0;     // gradient^weight pole<->belly
const vec3  uCloudsTint       = vec3(0.90, 0.78, 0.55);  // dusty tan clouds
const float uCloudsBoost      = 1.0;
const float uCloudShadows     = 0.5;
const float uCloudBoost       = 0.6;     // how strongly the lit cloud layer adds over ground

// Atmospheric scatter LUT (warm sunset_yellow ramp).
const vec3  uScatterColor    = vec3(1.0, 0.86, 0.6);
const float uScatterBoost    = 1.0;
const float uScatterIndirect = 0.18;
const float uScatterStretch  = 1.0;
const float uScatterCenterShift = 0.0;

void main() {
    vec3 N  = normalize(vWorldNormal);
    vec3 No = normalize(vObjPos);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(cam.sunDir.xyz);
    mat3 w2t = transpose(vTBN);

    // Light/ambient sourced from the engine Camera UBO (was the old PlanetFrame).
    // Sand albedo is moderately bright; a modest sun radiance boost keeps the lit
    // hemisphere reading warm in the HDR/ACES scene without blowing out the dunes.
    vec3 uLightColor = vec3(1.0, 0.96, 0.90) * 2.0;
    vec3 uAmbient    = cam.ambientCount.rgb;

    // ---- Dust clouds (two animated layers + self-shadow) ----
    vec3 cloudColor; float cloudShadow;
    cloudsTwoLayer(pc.tex[4], pc.tex[5], pc.tex[3], vUV,
                   uCloudsTop_ST, uCloudsMiddle_ST, uGradient_ST,
                   uCloudsTopSpeed, uCloudsMiddleSpeed, uCloudsBlendWeight,
                   uCloudsTint, uCloudsBoost, uCloudShadows,
                   L, w2t, pc.uTime, cloudColor, cloudShadow);
    vec3 cloudSpec = max(cloudColor, vec3(0.003));
    vec3 cloudsLit = standardSpecular(cloudColor, cloudSpec, 0.0, N, V, L, uLightColor, uAmbient);

    // ---- Ground (triplanar detail control + biome color blends) ----
    vec4 detail = triplanar(pc.tex[0], vObjPos, No, 5.0, uDetailTiling);
    float dX = detail.x, dY = detail.y, dZ = detail.z;  // R=base/spec, G=mtn, B=desert

    float desertT = saturate1((uDesertCoverage - dX * dZ) * uDesertFactors);
    vec3 desertResult = mix(uBaseColor * dX, uDesertColor * dZ, desertT);
    float mountT = saturate1((uMountainCoverage - dY) * uMountainFactors);
    vec3 terrain = saturate3(mix(saturate3(desertResult), dX * uMountainColor, mountT));

    vec4 nTri = triplanar(pc.tex[1], vObjPos, No, 5.0, uNormalTiling);
    vec3 nTan = normalize(unpackScaleNormal(nTri, uNormalScale));
    vec3 groundN = normalize(vTBN * nTan);

    float fdot = dot(V, groundN);
    vec3 fresnelColor = saturate1(pow(saturate1(1.0 - fdot), uFresnelPower)) * uFresnelMult * uFresnelColor;

    vec3 albedoCore   = saturate3(saturate3(terrain * terrain * uAlbedoBoost) + fresnelColor);
    vec3 groundAlbedo = cloudShadow * albedoCore;
    vec3 groundSpec   = dX * uSpecular * uSpecularColor;
    float groundSmooth = uSmoothness * cloudShadow;
    vec3 groundLit = standardSpecular(groundAlbedo, groundSpec, groundSmooth, groundN, V, L, uLightColor, uAmbient);

    // ---- Scatter + composite ----
    vec3 scatter = scatterTerm(pc.tex[2], N, L, V, uScatterCenterShift, uScatterStretch,
                               uScatterColor, uLightColor, uScatterBoost, uScatterIndirect);
    vec3 lit = saturate3(saturate3(cloudsLit * uCloudBoost) + groundLit);
    fragColor = vec4(saturate3(scatter * lit), 1.0);
}
