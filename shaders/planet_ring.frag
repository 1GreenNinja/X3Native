#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet_ring.frag  — FORGE3D Planets HD / Ring (planetary ring disc), ported to
//  the X3Native bindless pipeline. A flat annulus/disc mesh authored in OBJECT
//  space with the planet center at the origin and the planet surface radius == 1.0.
//  The ring is colored by a RADIAL strip lookup (inner edge -> outer edge) and
//  darkened by the planet's cylindrical cast shadow. STATIC (no time).
//
//    albedo = detail.rgb * RingTint.rgb * shadowTerm ;  alpha = detail.r * RingTint.a
//
//  BLEND CLASS: ALPHA  (SRC_ALPHA / ONE_MINUS_SRC_ALPHA, ZWrite Off, Cull Back).
//  Output is vec4(color, A) with A from the ring strip's red channel * tint alpha.
//
//  All FORGE3D PlanetParams (P.*) are replaced with hardcoded const values taken
//  from the ShaderLab defaults. The world->object matrix the reference read from a
//  UBO is here derived from the push-constant model matrix (inverse(mat3(model))).
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

// ---- Push constant (generalized — supersedes Moon's uvec4). 12 bindless texture
// slots + uTime; the engine fills these. Ring uses only tex[0] (the radial strip).
layout(push_constant) uniform PC {
    mat4  model;       // object -> world
    uint  tex[12];     // bindless texture indices
    float uTime;
    float _p0;
    uint  _p1;
    uint  _p2;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec3 vObjPos;     // REQUIRED: ring math is object-space
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec3 vViewDir;
layout(location = 5) in mat3 vTBN;
layout(location = 0) out vec4 fragColor;

// ---- saturate helper (inlined from planet_common.glsl) ----------------------
float saturate1(float x){ return clamp(x, 0.0, 1.0); }

// =============================================================================
//  RING material constants (Saturn-style icy ring). Hardcoded in place of the old
//  PlanetParams UBO (P.*). Tasteful defaults per the FORGE3D ShaderLab values.
//    uRingTint    : overall color/alpha multiply for the strip (slightly warm ice)
//    uRingSize    : outer radius (object space)
//    uRingOffset  : inner radius = uRingOffset + 1.0  (gap above the 1.0 surface)
//    uShadowVertex   : scales obj pos for the cylindrical shadow math
//    uShadowPenumbra : [0..1] shadow edge softness
// =============================================================================
const vec4  uRingTint       = vec4(1.0, 0.96, 0.88, 1.0);
const float uRingSize       = 2.5;   // outer radius
const float uRingOffset     = 0.3;   // inner radius = 1.3
const float uShadowVertex   = 1.0;
const float uShadowPenumbra = 0.15;

void main() {
    // World->object 3x3 from the push-constant model matrix (object->world). The
    // FORGE3D reference read this from a UBO (P.uWorldToObject); here we derive it.
    mat3 worldToObject = inverse(mat3(pc.model));

    // Light into object space (ASE ObjSpaceLightDir). sunDir = TOWARD the sun.
    vec3 objLit = normalize(worldToObject * cam.sunDir.xyz);

    // Radial UV along the ring (inner edge -> outer edge); sampled at v = 0.5.
    // The radial strip texture must use CLAMP_TO_EDGE so radialT outside [0,1]
    // clamps to the inner/outer edge color.
    float innerR  = uRingOffset + 1.0;
    float radialT = (length(vObjPos) - innerR) / (uRingSize - innerR);
    vec4  detail  = texture(textures[nonuniformEXT(pc.tex[0])], vec2(radialT, 0.5));

    // Planet cylindrical cast shadow. perpDist is the distance of the shaded point
    // from the planet->sun axis; points within ~1 unit and on the far side of the
    // planet (frontFace == 0) fall in shadow.
    vec3  p          = vObjPos * uShadowVertex;
    float perpDist   = length(cross(objLit, p));
    float shadowEdge = smoothstep(1.0 - uShadowPenumbra, 1.0 + uShadowPenumbra, perpDist);
    float frontFace  = clamp(dot(p, objLit), 0.0, 1.0);
    float shadowTerm = clamp(shadowEdge + frontFace, 0.0, 1.0);

    vec3  albedo = detail.rgb * uRingTint.rgb * shadowTerm;
    float alpha  = detail.r * uRingTint.a;
    fragColor = vec4(albedo, saturate1(alpha));
}
