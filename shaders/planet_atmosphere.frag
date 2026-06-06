#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_atmosphere.frag — FORGE3D Planets HD / Atmosphere (fresnel glow shell),
//  ported to the X3Native bindless pipeline. ADDITIVE emissive shell drawn on a
//  mesh slightly larger than the planet (planet.vert inflates verts along normal).
//  A wide scattering halo + a tight inner glow ring, both gated by analytic
//  fresnel masks, modulated by a day/night light term. NO noise, NO time —
//  pure fresnel + one horizon gradient-ramp lookup.
//
//    halo   = offsetScatter * pow(1-satNdv, scatterFactor) * scatterInt * rampTex*scatterColor
//    ring   = offsetRing    * saturate( pow(1-satNdv, glowFactor) * glowColor * glowInt )
//    light  = pow( saturate(N.L + max(backLimb,-0.22)*lightMul), lightExp )
//    glow   = (halo + ring) * light * lightAtten * uLightColor
//
//  BLEND CLASS: ADDITIVE (src ONE, dst ONE), depthWrite OFF, cull BACK.
//  The emissive RGB IS the result (premultiplied glow). Alpha is unused under
//  One/One; we emit A = 1.0 to mirror the reference port.
//
//  TEXTURE SLOT MAPPING (generalized push constant pc.tex[]):
//    pc.tex[0] = _AtmosphereSample  — horizon optical-depth gradient ramp (sRGB,
//                CLAMP_TO_EDGE), sampled at (uUVOffset + saturate(N.V)).
//    pc.tex[1..11] unused.
//
//  Helpers: this port needs NO triplanar / normal-unpack / scatter-LUT / cloud
//  helpers — only a single ramp lookup and analytic fresnel. saturate1/3 are
//  inlined from planet_common.glsl for self-containment.
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

// ---- Push constant (generalized) — must match planet.vert's updated PC.
layout(push_constant) uniform PC {
    mat4  model;
    uint  tex[12];   // [0]=_AtmosphereSample (rest unused for this type)
    float uTime;     // unused (atmosphere is static)
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

// ---- saturate helpers (inlined from planet_common.glsl) --------------------
float saturate1(float x){ return clamp(x, 0.0, 1.0); }
vec3  saturate3(vec3  x){ return clamp(x, 0.0, 1.0); }
vec4  saturate4(vec4  x){ return clamp(x, 0.0, 1.0); }

// =============================================================================
//  ATMOSPHERE shell constants. Hardcoded in place of the old PlanetParams UBO
//  (P.*), taken from the FORGE3D ShaderLab defaults (a tasteful blue-sky shell:
//  cyan-blue wide scatter halo + a brighter inner rim glow). Fixed-look first
//  pass; these get exposed as material params later.
// =============================================================================
const float uScatteringOffset    = 0.0;                 // gate bias (/10 then *1000 saturate)
const vec4  uScatteringColor      = vec4(0.30, 0.55, 1.0, 1.0); // sky-blue halo tint
const float uScatteringIntensity  = 1.0;
const float uScatteringFactor     = 2.0;                 // rim falloff exponent (wide)
const float uUVOffset             = 0.0;                 // ramp lookup bias
const float uGlowOffset           = 0.0;                 // inner-ring gate bias
const vec4  uGlowColor            = vec4(0.45, 0.70, 1.0, 1.0); // brighter inner rim
const float uGlowIntensity        = 1.0;
const float uGlowFactor           = 6.0;                 // tight rim falloff exponent
const float uLightMultiply        = 1.0;                 // back-limb forward-scatter weight
const float uLightExp             = 1.0;                 // day/night sharpness
const float uLightAtten           = 1.0;                 // shadow/range atten (1 = unshadowed)

void main() {
    vec3 N = normalize(vWorldNormal);
    vec3 V = normalize(vViewDir);          // toward camera (from planet.vert)
    vec3 L = normalize(cam.sunDir.xyz);    // toward the sun

    // Sun radiance from the engine (Atmosphere ramps are bright; no >1 boost
    // needed like the Moon's dark rock required).
    vec3 uLightColor = vec3(1.0, 0.96, 0.90);

    float ndv    = dot(N, V);
    float absNdv = abs(ndv);
    float satNdv = saturate1(ndv);

    // Near-binary analytic gates (*1000 then saturate) — keep the halo/ring on
    // the visible limb.
    float offsetScattering = saturate1(((uScatteringOffset / 10.0) + absNdv) * 1000.0);
    float offsetInnerRing  = saturate1((absNdv + (uGlowOffset / 10.0)) * 1000.0);

    // Wide scattering halo (gradient ramp indexed by rim). CLAMP_TO_EDGE sampler.
    vec2  scatterUV  = vec2(uUVOffset + satNdv);
    vec4  scatterTex = texture(textures[nonuniformEXT(pc.tex[0])], scatterUV) * uScatteringColor;
    float scatterFalloff = pow(1.0 - satNdv, uScatteringFactor);
    vec4  scatterMap = vec4(offsetScattering * scatterFalloff * uScatteringIntensity) * scatterTex;

    // Tight inner glow ring.
    float glowFalloff = pow(1.0 - satNdv, uGlowFactor);
    vec4  innerRing   = offsetInnerRing * saturate4(vec4(glowFalloff) * uGlowColor * uGlowIntensity);

    // Day/night light term (+ back-limb forward-scatter for the sunset crescent).
    float ndl      = dot(L, N);
    float backLimb = dot(-L, V);
    float litRaw   = ndl + (max(backLimb, -0.22) * uLightMultiply);
    float lightTerm = saturate1(pow(saturate1(litRaw), uLightExp));

    vec3 atmos = (scatterMap.rgb + innerRing.rgb) * lightTerm * uLightAtten * uLightColor;

    // ADDITIVE (One/One): rgb IS the premultiplied glow; alpha unused (emit 1.0).
    fragColor = vec4(atmos, 1.0);
}
