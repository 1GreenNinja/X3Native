#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_sun.frag  — FORGE3D Planets HD / Sun (procedural emissive star body)
//  EMISSIVE-ONLY (no scene lighting; the sun is its own source). Triplanar
//  surface map distorted by an animated flow map, Cool->Warm heat gradient driven
//  by the surface mask (.g), bright flakes (.r) scaled by Hot, fresnel rim, master
//  boost. Output is pure HDR emission -> bloom. Render into the HDR target; the
//  vertex corona pulse (sunVertexPulse) belongs in planet.vert if used.
//
//  Conventions: uTime seconds (vertex pulse uses uTime/20). uSunDir UNUSED here.
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
    float uDistortionUVTiling; float uDistortionUVSpeed; float uDistortionFactor;
    float uSunRTiling; float uSunGTiling; float uSurfaceSpeed; float uTriplanarFalloff;
    vec4  uCool; vec4 uWarm; vec4 uHot;
    float uSurfaceMult; float uSurfacePower; float uFlakesMult; float uFlakesPower;
    float uFresnelMult; float uFresnelPower; vec4 uFresnelColor; float uBoost;
} P;

layout(set = 3, binding = 0) uniform sampler2D _SurfaceMap;     // R=flakes, G=surface heat
layout(set = 3, binding = 1) uniform sampler2D _DistortionMap;  // RG flow

void main() {
    vec3 objN = normalize(vObjPos);
    vec3 V    = normalize(vViewDir);

    // ---- Flow distortion ----
    vec2 flow = flowDistortOffset(_DistortionMap, vObjPos, objN, P.uDistortionUVTiling,
                                  P.uTriplanarFalloff, vec2(F.uTime * P.uDistortionUVSpeed), P.uDistortionFactor);
    vec2 surfScroll = vec2(F.uTime * P.uSurfaceSpeed) + flow;

    // ---- Surface heat mask (.g) -> Cool/Warm gradient ----
    float surfaceMask = clamp(triplanar(_SurfaceMap, vObjPos, objN, P.uTriplanarFalloff, P.uSunGTiling, surfScroll), 0.0, 1.0).g;
    vec4 coolScaled = P.uCool * 100.0 * P.uCool.a;
    vec4 warmScaled = P.uWarm * 100.0 * P.uWarm.a;
    float heatT = pow(surfaceMask, P.uSurfacePower) * P.uSurfaceMult;  // unclamped (HDR)
    vec4 heatColor = mix(coolScaled, warmScaled, heatT);
    vec4 surfaceTerm = heatColor * surfaceMask;

    // ---- Flakes (.r) scaled by Hot ----
    float flakesMask = clamp(triplanar(_SurfaceMap, vObjPos, objN, P.uTriplanarFalloff, P.uSunRTiling, surfScroll), 0.0, 1.0).r;
    float flakes = pow(flakesMask, P.uFlakesPower) * P.uFlakesMult;
    vec4 flakesTerm = flakes * P.uHot * P.uHot.a;

    // ---- Fresnel rim (ASE used viewDir.z literally) ----
    float fres = clamp(pow(clamp(1.0 - V.z, 0.0, 1.0), P.uFresnelPower), 0.0, 1.0);
    vec4 fresnelTerm = (fres * P.uFresnelMult) * P.uFresnelColor;

    vec4 emission = max(vec4(0.0), (surfaceTerm + flakesTerm + fresnelTerm) * P.uBoost);
    fragColor = vec4(emission.rgb, 1.0); // HDR emissive -> bloom
}

// VERTEX corona pulse helper (call from planet.vert if you want the rippling
// silhouette). uTimeSlow = uTime/20.  Returns an object-space position offset.
//   vec3 sunVertexPulse(vec3 worldPos, vec3 objNormal, vec3 objCenterWS,
//                       vec3 camPos, float uTimeSlow, float tile, float speed,
//                       float power, float falloff){
//       vec3 toCam = normalize(camPos - worldPos);
//       float mask = pow(1.0 - abs(dot(objNormal, toCam)), falloff);
//       vec3 wave = sin((worldPos - objCenterWS) * tile + (speed * uTimeSlow));
//       return mask * (wave * power);
//   }
