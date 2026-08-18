#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_sun.frag  — FORGE3D Planets HD / Sun (procedural emissive star body),
//  ported to the X3Native bindless pipeline.
//
//  EMISSIVE-ONLY: the sun is its own light source — there is NO scene lighting,
//  NO scatter LUT, NO shadow term. The fragment outputs pure HDR emission which
//  the engine's bloom + ACES tonemap turn into a glowing star.
//
//    flow    = triplanar(_DistortionMap).rg * distortionFactor   (animated)
//    surfUV  = uTime*surfaceSpeed + flow
//    heatT   = pow(surfaceMask.g, surfacePow) * surfaceMult       (HDR, unclamped)
//    surface = mix(Cool*100*Cool.a, Warm*100*Warm.a, heatT) * surfaceMask
//    flakes  = pow(surfaceMask.r, flakesPow) * flakesMult * Hot*Hot.a
//    fresnel = pow(saturate(1 - V.z), fresnelPow) * fresnelMult * fresnelColor
//    emission= max(0, (surface + flakes + fresnel) * boost)
//
//  Helpers (triplanar / flowDistortOffset / saturate*) are INLINED from
//  planet_common.glsl, with triplanar + flowDistortOffset rewritten to sample the
//  engine's BINDLESS texture array by a uint texture index. ANIMATED via pc.uTime.
//
//  BLEND CLASS: OPAQUE (A = 1.0). The glow comes from HDR magnitude, not blending.
// =============================================================================

// ---- Bindless texture array (set0/binding0) — EXACTLY as mesh.frag declares it.
layout(set = 0, binding = 0) uniform sampler2D textures[];

// ---- Per-frame Camera UBO (set1/binding1) — MUST match mesh.frag's block exactly.
//      (Sun is emissive, so sunDir/camPos/ambient are unused here, but the block
//       layout must still match the bound descriptor.)
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

// ---- Push constant (model + texture indices + time) — generalized planet PC.
//      tex[0] = _SurfaceMap  (R = flakes, G = surface heat)
//      tex[1] = _DistortionMap (RG = flow)
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

// ---- saturate helpers ------------------------------------------------------
float saturate1(float x){ return clamp(x, 0.0, 1.0); }
vec3  saturate3(vec3  x){ return clamp(x, 0.0, 1.0); }
vec4  saturate4(vec4  x){ return clamp(x, 0.0, 1.0); }

// ---- OBJECT-SPACE TRIPLANAR (bindless) — samples textures[texIdx] over the three
// object-axis planes blended by pow(abs(objNormal), falloff). uvOffset carries the
// animated scroll/flow offset. Rewritten to take a uint bindless index.
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

// ---- FLOW-MAP / UV self-distortion (bindless): sample the (triplanar) flow
// texture, saturate, scale by `factor`; its RG warps the UV of a second lookup.
vec2 flowDistortOffset(uint flowTexIdx, vec3 objPos, vec3 objNormal,
                       float tiling, float falloff, vec2 flowScroll, float factor){
    vec4 f = clamp(triplanar(flowTexIdx, objPos, objNormal, falloff, tiling, flowScroll), 0.0, 1.0);
    return f.rg * factor;
}

// =============================================================================
//  SUN material constants (procedural emissive star). Hardcoded in place of the
//  old PlanetParams UBO (P.*). Values are tasteful FORGE3D-style defaults for a
//  hot yellow-white star; params get exposed later.
// =============================================================================
const float uDistortionUVTiling = 1.0;
const float uDistortionUVSpeed  = 0.02;   // slow flow scroll
const float uDistortionFactor   = 0.10;   // warp strength
const float uSunRTiling         = 1.0;    // flakes (.r) tiling
const float uSunGTiling         = 1.0;    // surface heat (.g) tiling
const float uSurfaceSpeed       = 0.015;  // surface UV scroll speed
const float uTriplanarFalloff   = 5.0;

// HDR heat ramp colors (alpha = intensity multiplier; *100 in the mix as ASE did).
const vec4  uCool   = vec4(0.90, 0.30, 0.05, 0.012);  // deep ember (low heat)
const vec4  uWarm   = vec4(1.00, 0.70, 0.25, 0.030);  // bright gold (high heat)
const vec4  uHot    = vec4(1.00, 0.95, 0.80, 0.040);  // white-hot flakes

const float uSurfaceMult  = 1.0;
const float uSurfacePower = 1.5;
const float uFlakesMult   = 2.0;
const float uFlakesPower   = 3.0;

const float uFresnelMult  = 1.2;
const float uFresnelPower = 3.0;
const vec4  uFresnelColor = vec4(1.0, 0.45, 0.10, 1.0);  // hot orange limb

const float uBoost = 1.6;   // master emissive boost

void main() {
    vec3 objN = normalize(vObjPos);
    vec3 V    = normalize(vViewDir);

    // ---- Flow distortion (animated) ----
    vec2 flow = flowDistortOffset(pc.tex[1], vObjPos, objN, uDistortionUVTiling,
                                  uTriplanarFalloff, vec2(pc.uTime * uDistortionUVSpeed),
                                  uDistortionFactor);
    vec2 surfScroll = vec2(pc.uTime * uSurfaceSpeed) + flow;

    // ---- Surface heat mask (.g) -> Cool/Warm HDR gradient ----
    float surfaceMask = saturate4(triplanar(pc.tex[0], vObjPos, objN,
                                  uTriplanarFalloff, uSunGTiling, surfScroll)).g;
    vec4  coolScaled  = uCool * 100.0 * uCool.a;
    vec4  warmScaled  = uWarm * 100.0 * uWarm.a;
    float heatT       = pow(surfaceMask, uSurfacePower) * uSurfaceMult;  // unclamped (HDR)
    vec4  heatColor   = mix(coolScaled, warmScaled, heatT);
    vec4  surfaceTerm = heatColor * surfaceMask;

    // ---- Flakes (.r) scaled by Hot ----
    float flakesMask = saturate4(triplanar(pc.tex[0], vObjPos, objN,
                                 uTriplanarFalloff, uSunRTiling, surfScroll)).r;
    float flakes     = pow(flakesMask, uFlakesPower) * uFlakesMult;
    vec4  flakesTerm = flakes * uHot * uHot.a;

    // ---- Fresnel rim (ASE used viewDir.z literally) ----
    float fres        = saturate1(pow(saturate1(1.0 - V.z), uFresnelPower));
    vec4  fresnelTerm = (fres * uFresnelMult) * uFresnelColor;

    vec4 emission = max(vec4(0.0), (surfaceTerm + flakesTerm + fresnelTerm) * uBoost);
    fragColor = vec4(emission.rgb, 1.0); // HDR emissive -> bloom (OPAQUE)
}
