#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_suncorona.frag  — FORGE3D Planets HD / Sun Corona (additive halo shell),
//  ported to the X3Native bindless pipeline (matches planet_moon.frag conventions).
//
//  ADDITIVE (Blend One One), ZWrite Off, Cull OFF, emissive-only. Drawn on a back-
//  shell / billboard around the sun. ALL layers come from ONE grayscale noise map
//  (_CoronaMap) sampled with polar-remapped + panned/rotated UVs: solar-storm
//  discharge filaments, a slow corona plume, radial soft masks, fresnel edge fade,
//  and an optional soft-particle depth fade. pc.uTime drives the animation
//  (Unity _Time.x == pc.uTime/20).
//
//  PIPELINE / BLEND CLASS: ADDITIVE (src ONE, dst ONE); depthWrite OFF; cull OFF.
//  fragColor.rgb = premultiplied glow; alpha is unused by the additive blend.
//  No depth buffer bound here, so the soft-particle depthFade is held at 1.0.
//
//  Helpers (rotateUV / polarCoord / polarHalf / coronaMask + the INV_2PI/TWO_PI
//  constants) are INLINED from planet_common.glsl / the reference port. The lone
//  _CoronaMap sampler is replaced by the engine's BINDLESS array indexed by a uint.
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

// ---- Push constant (generalized: model + up to 12 bindless texture indices + time).
//  SunCorona uses exactly ONE texture:
//    pc.tex[0] = _CoronaMap  (Sun/Textures/suncorona_01.png, grayscale R/G atlas)
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

const float PI      = 3.14159265358979323846;
const float TWO_PI  = 6.28318530717958647692;
const float INV_2PI = 0.15915494309189533577;

// ---- saturate helper -------------------------------------------------------
float saturate1(float x){ return clamp(x, 0.0, 1.0); }

// ---- ROTATE UV about (0.5,0.5) by angle = speed*time (ASE "Rotator"/"rotateUV").
vec2 rotateUV(vec2 uv, float speed, float time){
    uv -= 0.5;
    float s = sin(speed * time);
    float c = cos(speed * time);
    vec2 r;
    r.x = uv.x * c + uv.y * s;     // ASE HLSL mul(rowVec, ((c,-s),(s,c)))
    r.y = uv.x * (-s) + uv.y * c;
    return r + 0.5;
}

// ---- POLAR COORDINATES (ASE "Polar Coordinates"). Input uv2 = uv*2 (centered to
// ~[-1,1] via -1). Returns (tileX*angle01, tileY*radius).
vec2 polarCoord(vec2 uv2, float tileX, float tileY){
    vec2 p = uv2 - 1.0;
    float a = -atan(p.y, p.x);
    float ang = a * INV_2PI;
    if (a < 0.0) ang = (a + TWO_PI) * INV_2PI;     // wrap to [0,1)
    return vec2(tileX * ang, tileY * length(p));
}

// ---- Half-tiled polar variant used by the discharge layer (tile*0.5).
vec2 polarHalf(vec2 uv2, float tileX, float tileY){
    vec2 p = uv2 - 1.0;
    float a = -atan(p.y, p.x);
    float ang = a * INV_2PI; if (a < 0.0) ang = (a + TWO_PI) * INV_2PI;
    return vec2((tileX * 0.5) * ang, length(p) * (0.5 * tileY));
}

// ---- Radial soft mask: 1 - (k * |centered ellipse radius|)^e, clamped.
float coronaMask(vec2 uv, float wx, float wy, float k, float e){
    vec2 c = uv - 0.5; vec2 b = c * c;
    float r = sqrt(wx * b.x + wy * b.y);
    return clamp(1.0 - pow(k * r, e), 0.0, 1.0);
}

// ---- Bindless sampler convenience (one map, indexed by pc.tex[0]).
vec4 sampleCorona(vec2 uv){
    return texture(textures[nonuniformEXT(pc.tex[0])], uv);
}

// =============================================================================
//  SUNCORONA material constants. Hardcoded in place of the old PlanetParams UBO
//  (P.*); values are the FORGE3D ShaderLab defaults (tasteful fixed-look pass).
// =============================================================================
const vec3  uCoronaColor          = vec3(1.0, 0.55, 0.12);  // warm solar orange
const float uDischargePanSpeed    = 0.15;
const float uDischargeTileX       = 6.0;
const float uDischargeTileY       = 4.0;
const float uCoronaFluidTile      = 1.0;
const float uCoronaFluidInfluence = 0.10;
const float uSolarStormFalloff    = 2.0;
const float uSolarStormPower      = 4.0;
const float uCoronaSpeed          = 0.05;
const float uCoronaTileX          = 4.0;
const float uCoronaTileY          = 1.0;
const float uCoronaAmp            = 1.5;
const float uCoronaExp            = 2.0;
const float uCoronaBoost          = 1.0;
const float uCoronaFalloff        = 1.5;
const float uEdgeFadeFalloff      = 1.0;
const float uEdgeFadeBoost        = 1.0;

void main() {
    float tx  = pc.uTime / 20.0;    // Unity _Time.x
    // VIEW-FACING radial UV: center (0.5) at the sun's screen center, radius -> 0.5 at the
    // limb, so the ASE corona/polar math (authored for a camera-facing disc) forms a proper
    // halo at ANY viewing angle instead of being baked onto a fixed sphere patch.
    vec3  _Vv   = normalize(cam.camPos.xyz - vWorldPos);
    vec3  _Nw   = normalize(vWorldNormal);
    float _rad  = 1.0 - clamp(abs(dot(_Nw, _Vv)), 0.0, 1.0);      // 0 center -> 1 limb
    vec3  _sunC = vec3(pc.model[3][0], pc.model[3][1], pc.model[3][2]);
    vec3  _toC  = normalize(_sunC - cam.camPos.xyz);
    vec3  _Rx   = normalize(cross(_toC, vec3(0.0, 1.0, 0.0)));
    vec3  _Uy   = cross(_Rx, _toC);
    vec3  _toF  = normalize(vWorldPos - cam.camPos.xyz);
    vec2  _d2   = vec2(dot(_toF, _Rx), dot(_toF, _Uy));
    _d2 = (length(_d2) > 1e-4) ? normalize(_d2) : vec2(1.0, 0.0);
    vec2  uv  = clamp(0.5 + 0.5 * _rad * _d2, 0.0, 1.0);
    vec2  uv2 = uv * 2.0;
    vec2  panDir = vec2(0.0, -1.0);

    // cNoiseE: static-rotated radial streaks * cMaskC
    vec2 uvE = rotateUV(uv2, 4.0, 1.0);            // static 4.0 rad rotation
    float cMaskC = coronaMask(uv, 0.65, 0.65, 3.75, 3.0);
    float cNoiseE = pow(sampleCorona(uvE).r, 1.25) * 2.0 * cMaskC;

    // Solar storm: two polar discharge layers, fluid-distorted.
    float panAmt = uDischargePanSpeed * tx;
    vec2 uvA = polarHalf(uv2, uDischargeTileX, uDischargeTileY) + panAmt * panDir;
    float cNoiseA = sampleCorona(uvA).g;
    vec2 uvFluid = rotateUV(uv * uCoronaFluidTile * 4.0, tx * 0.2, 1.0);
    float cNoiseC = sampleCorona(uvFluid).r * uCoronaFluidInfluence;
    vec2 uvB = polarCoord(uv2, uDischargeTileX, uDischargeTileY) + panAmt * panDir;
    float cNoiseB = sampleCorona(uvB + cNoiseC).r;
    float sStorm = pow(cNoiseA * cNoiseB, uSolarStormFalloff) * uSolarStormPower;

    // Corona ring masks + plume.
    float cMaskA = (1.0 - coronaMask(uv, 1.0, 1.0, 4.0, 3.0)) * 3.5;
    float maskBinner = coronaMask(uv, 1.0, 1.0, 2.25, 0.01);
    float cMaskB = clamp(pow(cMaskA * maskBinner * uCoronaBoost, uCoronaFalloff), 0.0, 1.0);
    vec2 uvC = polarCoord(uv2, uCoronaTileX, uCoronaTileY) + (tx * uCoronaSpeed) * panDir;
    float corona = pow(sampleCorona(uvC).g * uCoronaAmp, uCoronaExp);

    // Edge fresnel fade (no depth buffer bound: dFade = 1).
    float dNV = dot(normalize(vViewDir), normalize(vWorldNormal));
    float eFade = clamp(pow(abs(dNV), uEdgeFadeFalloff) * uEdgeFadeBoost, 0.0, 1.0);
    float dFade = 1.0; // soft-particle depth fade: wire scene depth here when available

    float combined = (cNoiseE * sStorm) + (cMaskB + corona);
    vec3 emission = uCoronaColor * combined * 5.0 * cMaskB * dFade * eFade;
    fragColor = vec4(emission, 1.0); // ADDITIVE: premultiplied glow, alpha unused
}
