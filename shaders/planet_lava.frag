#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_lava.frag  — FORGE3D Planets HD / Lava (molten world), ported to the
//  X3Native bindless pipeline. Object-space triplanar Height/Detail/Magma/Normal
//  + Ramp3 rock color + glowing magma veins revealed by a lava mask + animated
//  flow-distorted molten color (HDR emissive) + fresnel rim + atmospheric scatter
//  LUT. Lava self-emits on the night side via a small _LavaPasstrough floor.
//  ANIMATED (cloud/molten flow uses pc.uTime). HDR emissive -> bloom.
//
//    termA = saturate(scatter * (baseColor + surfWhite*fresnel))
//    termB = (lavaColor * lavaMask) * saturate(passthrough + surfWhite)
//    final = termA + termB
//
//  Helpers (triplanar / unpackScaleNormal / saturate* / linstep / ramp3 /
//  flowDistortOffset / scatterTerm / standardSpecular) are INLINED from
//  planet_common.glsl, with the texture-sampling helpers rewritten to read the
//  engine's BINDLESS array by a uint texture index.
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

// ---- Push constant (generalized) — model + 12 bindless texture indices + time.
//  LAVA texture-slot mapping (document):
//    pc.tex[0] = _HeightMap     (Lava/Textures/lava_0N.png, R)            — sRGB
//    pc.tex[1] = _DetailMap     (Lava/Textures/lavadetail_0N.png, R,G)    — linear
//    pc.tex[2] = _MagmaMap      (Lava/Textures/lavadetail_*.png, R)       — linear
//    pc.tex[3] = _NormalMap     (Ice/Textures/ice_04_normal.png, RG)      — linear
//    pc.tex[4] = _DistortionMap (Lava/Textures/lavadistmap.png, RGB+RG)   — sRGB
//    pc.tex[5] = _ScatterMap    (Atmosphere/sunset_red_04.png, RGB)       — sRGB, CLAMP_TO_EDGE
//    pc.tex[6..11] unused.
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

// ---- ASE "Ramp3": two-stage clamped gradient. t pre-divided by scale at call site.
vec3 ramp3(vec3 low, vec3 mid, vec3 high, float t){
    vec3 c = mix(low, mid,  clamp(t,       0.0, 1.0));
    return  mix(c,   high, clamp(t - 1.0, 0.0, 1.0));
}

// ---- Tangent-space normal unpack (FORGE3D ships plain RGB normal maps; xy in rg).
vec3 unpackScaleNormal(vec4 packed, float scale){
    vec3 n;
    n.xy = (packed.rg * 2.0 - 1.0) * scale;
    n.z  = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// ---- OBJECT-SPACE TRIPLANAR (bindless): samples textures[texIdx] over the three
// object-axis planes blended by pow(abs(objNormal), falloff). uvOffset carries any
// animated scroll/flow offset (vec2(0) if static). Mirrors the ASE Spherical/Object
// triplanar in planet_common.glsl, rewritten to take a uint bindless index.
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
// saturate, scale by `factor`; its RG warps the UV of a second lookup.
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
//  LAVA material constants (molten world). Hardcoded in place of the old
//  PlanetParams UBO (P.*), taken from the FORGE3D ShaderLab defaults — tasteful
//  fixed-look first pass. Params get exposed later.
// =============================================================================
// Scatter (sunset_red_04 LUT).
const vec3  uScatterColor    = vec3(1.0, 0.75, 0.55);
const float uScatterBoost    = 1.0;
const float uScatterIndirect = 0.15;
const float uScatterStretch  = 1.0;   // 1 = identity LUT coord
const float uScatterCenterShift = 0.0;

// Rock color ramp (cooled basalt -> ash -> warm rock).
const vec3  uLavaLow  = vec3(0.045, 0.035, 0.030);
const vec3  uLavaMid  = vec3(0.150, 0.090, 0.070);
const vec3  uLavaHigh = vec3(0.320, 0.220, 0.170);

// Height/detail ramp factors.
const float uLavaFactorsX = 0.35;   // ramp low edge
const float uLavaFactorsY = 0.85;   // ramp high edge
const float uLavaFactorsZ = 1.0;    // ramp scale divisor
const float uLavaDetail   = 0.10;   // detail bias on the edges
const float uDetailExp    = 1.0;
const float uHeightDetail = 1.0;
const float uDetailTiling = 1.0;
const float uHeightTiling = 1.0;

// Normal.
const float uNormalScale  = 1.0;
const float uNormalTiling = 1.0;

// Specular workflow.
const vec3  uSpecularColor = vec3(1.0);
const float uSpecular      = 0.08;
const float uSpecularBoost = 1.0;
const float uSmoothness    = 0.20;

// Fresnel rim (faint warm haze on the limb).
const float uFresnelMult  = 0.20;
const float uFresnelPower  = 3.0;
const vec3  uFresnelColor  = vec3(1.0, 0.45, 0.20);

// Magma veins.
const float uMagmaTiling = 1.0;
const float uMagmaPower  = 2.0;
const float uMagmaBoost  = 2.0;
const float uMagmaGlow   = 6.0;     // HDR -> bloom
const vec3  uMagmaColorMin = vec3(0.80, 0.10, 0.02);  // deep red veins
const vec3  uMagmaColorMax = vec3(1.40, 0.70, 0.20);  // bright orange/yellow cores (HDR)

// Lava mask (crack reveal + view weighting).
const float uLavaMaskTiling  = 1.0;
const float uLavaMaskFactorsX = 4.0;  // crack edge power
const float uLavaMaskFactorsY = 1.5;  // crack boost
const float uLavaMaskPower   = 1.0;   // view-term power
const float uLavaMaskBoost   = 1.0;   // view-term boost
const float uLavaPasstrough  = 0.15;  // night-side self-emission floor

// Animated molten flow (distortion map).
const float uDistortionUVTiling = 1.0;
const float uDistortionUVSpeed  = 0.020;
const float uDistortionTiling   = 1.0;
const float uDistortionSpeed    = 0.010;
const float uDistortionFactor   = 0.10;

void main() {
    vec3 Nw = normalize(vWorldNormal);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(cam.sunDir.xyz);
    vec3 oP = vObjPos;
    vec3 oN = normalize(vObjPos);

    // Light/ambient sourced from the engine Camera UBO. The rock albedo is dark
    // basalt; boost the sun radiance so the lit hemisphere reads (HDR + ACES
    // absorb it). The molten emissive path is independent of this.
    vec3 uLightColor = vec3(1.0, 0.96, 0.90) * 3.0;
    vec3 uAmbient    = cam.ambientCount.rgb;

    // ---- Detail ----
    vec4 detailTri = clamp(triplanar(pc.tex[1], oP, oN, 5.0, uDetailTiling), 0.0, 1.0);
    float detailX   = detailTri.r;
    float detailTex = saturate1(uDetailExp * pow(detailTri.r * detailTri.g, uDetailExp) * 5000.0);

    // ---- Height -> remap -> rock color ramp ----
    vec4 heightTri = clamp(triplanar(pc.tex[0], oP, oN, 5.0, uHeightTiling), 0.0, 1.0);
    float detaledHeight = saturate1(detailTex * saturate1(heightTri.r * uHeightDetail));
    float lo  = saturate1(uLavaFactorsX - uLavaDetail);
    float hi  = saturate1(uLavaFactorsY - uLavaDetail);
    float h01 = linstep(lo, hi, detaledHeight);
    vec3 rockAlbedo = ramp3(uLavaLow, uLavaMid, uLavaHigh, h01 / max(uLavaFactorsZ, 1e-4));

    // ---- Normal ----
    vec4 nTri = triplanar(pc.tex[3], oP, oN, 1.0, uNormalTiling);
    vec3 nTS  = unpackScaleNormal(nTri, uNormalScale);
    vec3 N    = normalize(vTBN * nTS);

    // ---- Rock lighting + white directional term ----
    vec3 specColor = saturate3(vec3(detailTex * uSpecular) * uSpecularColor * uSpecularBoost);
    float rockSmooth = (1.0 - detailTex) * uSmoothness;
    vec3 baseColor = standardSpecular(rockAlbedo, specColor, rockSmooth, N, V, L, uLightColor, uAmbient);
    vec3 surfWhite = max(dot(N, L), 0.0) * uLightColor;

    // ---- Fresnel ----
    float fr = saturate1(pow(saturate1(1.0 - dot(V, N)), uFresnelPower));
    vec3 fresnel = saturate3(fr * uFresnelMult * uFresnelColor * detailX);

    // ---- Magma veins + lava mask ----
    vec4 magmaTri = clamp(triplanar(pc.tex[2], oP, oN, 5.0, uMagmaTiling), 0.0, 1.0);
    float magmaR = saturate1(saturate1(pow(magmaTri.r, uMagmaPower)) * uMagmaBoost * magmaTri.r);
    float lavaMaskMap = clamp(triplanar(pc.tex[0], oP, oN, 2.0, uLavaMaskTiling).r, 0.0, 1.0);
    float crackTerm = saturate1(saturate1(pow(saturate1(1.0 - lavaMaskMap), uLavaMaskFactorsX)) * uLavaMaskFactorsY);
    float viewTerm  = saturate1(saturate1(pow(max(dot(V, N), 0.0), uLavaMaskPower)) * uLavaMaskBoost);
    float lavaMask  = saturate1(magmaR + crackTerm * viewTerm);
    vec3  magmaColor = mix(uMagmaColorMin, uMagmaColorMax, lavaMask);

    // ---- Animated flow-distorted molten color ----
    vec2 warp = flowDistortOffset(pc.tex[4], oP, oN, uDistortionUVTiling, 5.0,
                                  vec2(pc.uTime * uDistortionUVSpeed), uDistortionFactor);
    vec2 flowOffset = vec2(pc.uTime * uDistortionSpeed) + warp;
    vec4 flowTri = clamp(triplanar(pc.tex[4], oP, oN, 5.0, uDistortionTiling, flowOffset), 0.0, 1.0);
    vec3 lavaColor = flowTri.rgb * magmaColor * uMagmaGlow;

    // ---- Scatter (geometric normal) + composite ----
    vec3 scatter = scatterTerm(pc.tex[5], Nw, L, V, uScatterCenterShift, uScatterStretch,
                               uScatterColor, uLightColor, uScatterBoost, uScatterIndirect);
    vec3 termA = saturate3(scatter * (baseColor + surfWhite * fresnel));
    vec3 passthrough = saturate3(vec3(uLavaPasstrough) + surfWhite);
    vec3 termB = (lavaColor * lavaMask) * passthrough;
    fragColor = vec4(termA + termB, 1.0);  // HDR emissive -> bloom; keep float target
}
