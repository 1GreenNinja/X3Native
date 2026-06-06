#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_lava.frag  — FORGE3D Planets HD / Lava (molten world)
//  Triplanar detail/height/magma/normal -> rock Ramp3 color + glowing magma veins
//  revealed by a lava mask, animated flow-distorted molten color (HDR emissive),
//  fresnel rim, atmospheric scatter LUT. Lava self-emits on the night side via a
//  small _LavaPasstrough floor. Magma boost/glow are huge (HDR -> bloom).
//
//  final = saturate(scatter*(rock + surfWhite*fresnel)) + (lavaColor*lavaMask)*saturate(passthrough+surfWhite)
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
    vec4  uScatterColor; float uScatterBoost; float uScatterIndirect; float uScatterStretch; float uScatterCenterShift;
    vec4  uLavaLow; vec4 uLavaMid; vec4 uLavaHigh;
    float uLavaFactorsX; float uLavaFactorsY; float uLavaFactorsZ; float uLavaDetail;
    float uDetailExp; float uHeightDetail; float uDetailTiling; float uHeightTiling;
    float uNormalScale; float uNormalTiling;
    vec4  uSpecularColor; float uSpecular; float uSpecularBoost; float uSmoothness;
    float uFresnelMult; float uFresnelPower; vec4 uFresnelColor;
    float uMagmaTiling; float uMagmaPower; float uMagmaBoost; float uMagmaGlow;
    vec4  uMagmaColorMin; vec4 uMagmaColorMax;
    float uLavaMaskTiling; float uLavaMaskFactorsX; float uLavaMaskFactorsY; float uLavaMaskPower; float uLavaMaskBoost;
    float uLavaPasstrough;
    float uDistortionUVTiling; float uDistortionUVSpeed; float uDistortionTiling; float uDistortionSpeed; float uDistortionFactor;
} P;

layout(set = 3, binding = 0) uniform sampler2D _HeightMap;
layout(set = 3, binding = 1) uniform sampler2D _ScatterMap;
layout(set = 3, binding = 2) uniform sampler2D _DetailMap;
layout(set = 3, binding = 3) uniform sampler2D _MagmaMap;
layout(set = 3, binding = 4) uniform sampler2D _NormalMap;
layout(set = 3, binding = 5) uniform sampler2D _DistortionMap;

void main() {
    vec3 Nw = normalize(vWorldNormal);
    vec3 V  = normalize(vViewDir);
    vec3 L  = normalize(F.uSunDir);
    vec3 oP = vObjPos;
    vec3 oN = normalize(vObjPos);

    // ---- Detail ----
    vec4 detailTri = clamp(triplanar(_DetailMap, oP, oN, 5.0, P.uDetailTiling), 0.0, 1.0);
    float detailX = detailTri.r;
    float detailTex = saturate1(P.uDetailExp * pow(detailTri.r * detailTri.g, P.uDetailExp) * 5000.0);

    // ---- Height -> remap ----
    vec4 heightTri = clamp(triplanar(_HeightMap, oP, oN, 5.0, P.uHeightTiling), 0.0, 1.0);
    float detaledHeight = saturate1(detailTex * saturate1(heightTri.r * P.uHeightDetail));
    float lo = saturate1(P.uLavaFactorsX - P.uLavaDetail);
    float hi = saturate1(P.uLavaFactorsY - P.uLavaDetail);
    float h01 = linstep(lo, hi, detaledHeight);
    vec3 rockAlbedo = ramp3(P.uLavaLow.rgb, P.uLavaMid.rgb, P.uLavaHigh.rgb, h01 / max(P.uLavaFactorsZ, 1e-4));

    // ---- Normal ----
    vec4 nTri = triplanar(_NormalMap, oP, oN, 1.0, P.uNormalTiling);
    vec3 nTS  = unpackScaleNormal(nTri, P.uNormalScale);
    vec3 N    = normalize(vTBN * nTS);

    // ---- Rock lighting + white directional term ----
    vec3 specColor = saturate3(detailTex * P.uSpecular * P.uSpecularColor.rgb * P.uSpecularBoost);
    float rockSmooth = (1.0 - detailTex) * P.uSmoothness;
    vec3 baseColor = standardSpecular(rockAlbedo, specColor, rockSmooth, N, V, L, F.uLightColor, F.uAmbient);
    vec3 surfWhite = max(dot(N, L), 0.0) * F.uLightColor;

    // ---- Fresnel ----
    float fr = saturate1(pow(saturate1(1.0 - dot(V, N)), P.uFresnelPower));
    vec3 fresnel = saturate3(fr * P.uFresnelMult * P.uFresnelColor.rgb * detailX);

    // ---- Magma veins + lava mask ----
    vec4 magmaTri = clamp(triplanar(_MagmaMap, oP, oN, 5.0, P.uMagmaTiling), 0.0, 1.0);
    float magmaR = saturate1(saturate1(pow(magmaTri.r, P.uMagmaPower)) * P.uMagmaBoost * magmaTri.r);
    float lavaMaskMap = clamp(triplanar(_HeightMap, oP, oN, 2.0, P.uLavaMaskTiling).r, 0.0, 1.0);
    float crackTerm = saturate1(saturate1(pow(saturate1(1.0 - lavaMaskMap), P.uLavaMaskFactorsX)) * P.uLavaMaskFactorsY);
    float viewTerm  = saturate1(saturate1(pow(max(dot(V, N), 0.0), P.uLavaMaskPower)) * P.uLavaMaskBoost);
    float lavaMask  = saturate1(magmaR + crackTerm * viewTerm);
    vec3  magmaColor = mix(P.uMagmaColorMin.rgb, P.uMagmaColorMax.rgb, lavaMask);

    // ---- Animated flow molten color ----
    vec2 warp = flowDistortOffset(_DistortionMap, oP, oN, P.uDistortionUVTiling, 5.0,
                                  vec2(F.uTime * P.uDistortionUVSpeed), P.uDistortionFactor);
    vec2 flowOffset = vec2(F.uTime * P.uDistortionSpeed) + warp;
    vec4 flowTri = clamp(triplanar(_DistortionMap, oP, oN, 5.0, P.uDistortionTiling, flowOffset), 0.0, 1.0);
    vec3 lavaColor = flowTri.rgb * magmaColor * P.uMagmaGlow;

    // ---- Scatter (geometric normal) + composite ----
    vec3 scatter = scatterTerm(_ScatterMap, Nw, L, V, P.uScatterCenterShift, P.uScatterStretch,
                               P.uScatterColor.rgb, F.uLightColor, P.uScatterBoost, P.uScatterIndirect);
    vec3 termA = saturate3(scatter * (baseColor + surfWhite * fresnel));
    vec3 passthrough = saturate3(vec3(P.uLavaPasstrough) + surfWhite);
    vec3 termB = (lavaColor * lavaMask) * passthrough;
    fragColor = vec4(termA + termB, 1.0);  // HDR emissive -> bloom; keep float target
}
