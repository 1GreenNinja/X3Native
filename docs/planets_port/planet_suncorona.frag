#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_suncorona.frag  — FORGE3D Planets HD / Sun Corona (additive halo shell)
//  ADDITIVE (Blend One One), ZWrite Off, Cull OFF, emissive-only. Drawn on a back-
//  shell / billboard around the sun. All layers come from ONE grayscale noise map
//  (_CoronaMap) sampled with polar-remapped + panned/rotated UVs: solar-storm
//  discharge filaments, a slow corona plume, radial soft masks, fresnel edge fade,
//  and an optional soft-particle depth fade. uTime drives the animation
//  (Unity _Time.x == uTime/20).
//
//  PIPELINE: blend = ADDITIVE (src ONE, dst ONE); depthWrite OFF; cull OFF.
//  If no depth buffer is bound, pass a huge sceneEyeDepth so depthFade -> 1.
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
    vec4  uCoronaColor;
    float uDischargePanSpeed; float uDischargeTileX; float uDischargeTileY;
    float uCoronaFluidTile; float uCoronaFluidInfluence;
    float uSolarStormFalloff; float uSolarStormPower;
    float uCoronaSpeed; float uCoronaTileX; float uCoronaTileY; float uCoronaAmp; float uCoronaExp; float uCoronaBoost; float uCoronaFalloff;
    float uDepthFade; float uEdgeFadeFalloff; float uEdgeFadeBoost;
} P;

layout(set = 3, binding = 0) uniform sampler2D _CoronaMap;

float coronaMask(vec2 uv, float wx, float wy, float k, float e){
    vec2 c = uv - 0.5; vec2 b = c * c;
    float r = sqrt(wx * b.x + wy * b.y);
    return clamp(1.0 - pow(k * r, e), 0.0, 1.0);
}
vec2 polarHalf(vec2 uv2, float tileX, float tileY){
    vec2 p = uv2 - 1.0;
    float a = -atan(p.y, p.x);
    float ang = a * INV_2PI; if (a < 0.0) ang = (a + TWO_PI) * INV_2PI;
    return vec2((tileX * 0.5) * ang, length(p) * (0.5 * tileY));
}

void main() {
    float tx  = F.uTime / 20.0;     // Unity _Time.x
    vec2  uv  = vUV;
    vec2  uv2 = uv * 2.0;
    vec2  panDir = vec2(0.0, -1.0);

    // cNoiseE: static-rotated radial streaks * cMaskC
    vec2 uvE = rotateUV(uv2, 4.0, 1.0);            // static 4.0 rad rotation
    float cMaskC = coronaMask(uv, 0.65, 0.65, 3.75, 3.0);
    float cNoiseE = pow(texture(_CoronaMap, uvE).r, 1.25) * 2.0 * cMaskC;

    // Solar storm: two polar discharge layers, fluid-distorted.
    float panAmt = P.uDischargePanSpeed * tx;
    vec2 uvA = polarHalf(uv2, P.uDischargeTileX, P.uDischargeTileY) + panAmt * panDir;
    float cNoiseA = texture(_CoronaMap, uvA).g;
    vec2 uvFluid = rotateUV(uv * P.uCoronaFluidTile * 4.0, tx * 0.2, 1.0);
    float cNoiseC = texture(_CoronaMap, uvFluid).r * P.uCoronaFluidInfluence;
    vec2 uvB = polarCoord(uv2, P.uDischargeTileX, P.uDischargeTileY) + panAmt * panDir;
    float cNoiseB = texture(_CoronaMap, uvB + cNoiseC).r;
    float sStorm = pow(cNoiseA * cNoiseB, P.uSolarStormFalloff) * P.uSolarStormPower;

    // Corona ring masks + plume.
    float cMaskA = (1.0 - coronaMask(uv, 1.0, 1.0, 4.0, 3.0)) * 3.5;
    float maskBinner = coronaMask(uv, 1.0, 1.0, 2.25, 0.01);
    float cMaskB = clamp(pow(cMaskA * maskBinner * P.uCoronaBoost, P.uCoronaFalloff), 0.0, 1.0);
    vec2 uvC = polarCoord(uv2, P.uCoronaTileX, P.uCoronaTileY) + (tx * P.uCoronaSpeed) * panDir;
    float corona = pow(texture(_CoronaMap, uvC).g * P.uCoronaAmp, P.uCoronaExp);

    // Edge fresnel fade (no depth buffer: dFade=1).
    float dNV = dot(normalize(vViewDir), normalize(vWorldNormal));
    float eFade = clamp(pow(abs(dNV), P.uEdgeFadeFalloff) * P.uEdgeFadeBoost, 0.0, 1.0);
    float dFade = 1.0; // soft-particle depth fade: wire scene depth here when available

    float combined = (cNoiseE * sStorm) + (cMaskB + corona);
    vec3 emission = P.uCoronaColor.rgb * combined * 5.0 * cMaskB * dFade * eFade;
    fragColor = vec4(emission, 1.0); // ADDITIVE: alpha unused
}
