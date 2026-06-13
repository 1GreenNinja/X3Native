#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_gas.frag  — FORGE3D Planets HD / Gas (gas giant)
//  Animated flow-distorted banded height/albedo (panning UVs + distortion map),
//  fresnel rim, specular-workflow PBR (spec from height tex, smoothness from
//  inverse height), atmospheric scatter LUT keyed on (N.L, N.V).
//  NOTE: Gas samples the height/distortion maps with LAT-LONG UV panners (not
//  triplanar) — the bands wrap around the equator. uTime drives two panners.
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
    vec4  uHeightMap_ST; vec4 uUVDistortionMap_ST;
    vec4  uUVSpeed; vec4 uUVDistortionSpeed;   // .xy used
    vec4  uTint; vec4 uSpecularTint; vec4 uFresnelColor;
    float uSpecular; float uSmoothness; float uFresnelPower; float uFresnelMult;
    float uUVDistortion;
    vec4  uScatterColor; float uScatterBoost; float uScatterIndirect; float uScatterStretch; float uScatterCenterShift;
} P;

layout(set = 3, binding = 0) uniform sampler2D _HeightMap;        // RGB color bands (albedo+spec)
layout(set = 3, binding = 1) uniform sampler2D _ScatterMap;
layout(set = 3, binding = 2) uniform sampler2D _UVDistortionMap;  // .r flow offset

void main() {
    vec3 N = normalize(vWorldNormal);
    vec3 V = normalize(vViewDir);
    vec3 L = normalize(F.uSunDir);

    // ---- Flow distortion -> animated banded albedo ----
    vec2 uvDist   = applyST(vUV, P.uUVDistortionMap_ST);
    vec2 panner11 = uvDist + F.uTime * P.uUVDistortionSpeed.xy;
    float distortion = texture(_UVDistortionMap, panner11).r * P.uUVDistortion;

    vec2 uvHeight = applyST(vUV, P.uHeightMap_ST);
    vec2 panner39 = uvHeight + F.uTime * P.uUVSpeed.xy;
    vec4 heightTex = texture(_HeightMap, panner39 + vec2(distortion));

    // ---- Fresnel (1 - N.V) ----
    float NoV = saturate1(dot(N, V));
    vec3 fresnel = saturate1(pow(saturate1(1.0 - NoV), P.uFresnelPower)) * P.uFresnelMult * P.uFresnelColor.rgb;

    // ---- PBR inputs ----
    vec3  albedo     = fresnel + (P.uTint.rgb * heightTex.rgb * 2.0);
    vec3  specColor  = heightTex.rgb * P.uSpecular * P.uSpecularTint.rgb;
    float smoothness = (1.0 - heightTex.r) * P.uSmoothness;
    vec3  lit = standardSpecular(albedo, specColor, smoothness, N, V, L, F.uLightColor, F.uAmbient);

    // ---- Scatter (raw N.V on Y, can be negative — faithful) ----
    vec3 scatter = scatterTerm(_ScatterMap, N, L, V, P.uScatterCenterShift, P.uScatterStretch,
                               P.uScatterColor.rgb, F.uLightColor, P.uScatterBoost, P.uScatterIndirect);
    fragColor = vec4(saturate3(scatter * lit), 1.0);
}
