#version 450
// #include "planet_common.glsl"
// =============================================================================
//  planet_ring.frag  — FORGE3D Planets HD / Ring (planetary ring disc)
//  ALPHA-BLEND (SRC_ALPHA / ONE_MINUS_SRC_ALPHA), ZWrite Off, Cull Back, queue
//  Transparent. A flat annulus/disc mesh authored in OBJECT space with the planet
//  center at the origin and the planet surface radius == 1.0. The ring is colored
//  by a RADIAL strip lookup (inner edge -> outer edge), and darkened by the
//  planet's cylindrical cast shadow.
//
//  albedo = detail.rgb * RingTint.rgb * shadowTerm ;  alpha = detail.r * RingTint.a
// =============================================================================

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec3 vObjPos;     // REQUIRED: ring math is object-space
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
    vec4  uRingTint;
    float uRingSize;        // outer radius (object space)
    float uRingOffset;      // inner radius = uRingOffset + 1.0
    float uShadowVertex;    // scales obj pos for shadow math
    float uShadowPenumbra;  // [0..1] shadow edge softness
    mat4  uWorldToObject;   // world->object (light dir into object space)
} P;

layout(set = 3, binding = 0) uniform sampler2D _DetailMap; // radial strip (CLAMP_TO_EDGE)

void main() {
    // Light into object space (ASE ObjSpaceLightDir).
    vec3 objLit = normalize(mat3(P.uWorldToObject) * F.uSunDir);

    // Radial UV along the ring.
    float innerR  = P.uRingOffset + 1.0;
    float radialT = (length(vObjPos) - innerR) / (P.uRingSize - innerR);
    vec4  detail  = texture(_DetailMap, vec2(radialT, 0.5));

    // Planet cylindrical cast shadow.
    vec3 p = vObjPos * P.uShadowVertex;
    float perpDist = length(cross(objLit, p));
    float shadowEdge = smoothstep(1.0 - P.uShadowPenumbra, 1.0 + P.uShadowPenumbra, perpDist);
    float frontFace  = clamp(dot(p, objLit), 0.0, 1.0);
    float shadowTerm = clamp(shadowEdge + frontFace, 0.0, 1.0);

    vec3  albedo = detail.rgb * P.uRingTint.rgb * shadowTerm;
    float alpha  = detail.r * P.uRingTint.a;
    fragColor = vec4(albedo, clamp(alpha, 0.0, 1.0));
}
