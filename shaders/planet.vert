#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =============================================================================
//  planet.vert  —  dedicated planet body vertex shader (FORGE3D Moon port).
//
//  Drives a UV-SPHERE authored in OBJECT space as a UNIT sphere (radius 1,
//  centered at the origin). The model matrix (push constant) scales/positions
//  it in the world. Object-space position == object-space normal direction,
//  which is exactly what the ASE triplanar samplers need.
//
//  ADDITIVE planet pipeline: instead of the mesh path's per-object SSBO row,
//  the model matrix + the four+one bindless texture indices ride in a PUSH
//  CONSTANT (set0 bindless texture array + set1 Camera UBO are reused as-is).
//
//  Outputs (consumed by planet_moon.frag):
//    vWorldPos    world-space position
//    vWorldNormal world-space normal (normalized)
//    vObjPos      OBJECT-space position (unit sphere; == object normal dir)
//    vUV          lat-long UV (longitude, latitude)
//    vViewDir     world-space direction TOWARD the camera
//    vTBN         tangent->world basis (columns T,B,N) for tangent normal maps
// =============================================================================

// Generalized planet push constant (shared by ALL planet types). planet.vert only
// reads pc.model; the per-type fragment shaders read their textures from pc.tex[].
layout(push_constant) uniform PC {
    mat4  model;       // object -> world
    uint  tex[12];     // up to 12 bindless texture indices (per-type slot mapping)
    float uTime;       // animation time (seconds)
    float _p0;
    uint  _p1;
    uint  _p2;
} pc;

// Per-frame Camera UBO (set1/binding1) — MUST match mesh.frag's block exactly.
struct PointLight { vec4 posRange; vec4 colorPad; };
const int kMaxPointLights = 64;
layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 ambientCount;
    PointLight lights[kMaxPointLights];
    vec4 camPos;          // xyz = camera world position
    vec4 sunDir;          // xyz = direction TOWARD the sun
} cam;

layout(location = 0) in vec3 inPos;     // unit-sphere object position
layout(location = 1) in vec3 inNormal;  // unit-sphere object normal (== normalize(inPos))
layout(location = 2) in vec2 inUV;      // baked lat-long UV

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vWorldNormal;
layout(location = 2) out vec3 vObjPos;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec3 vViewDir;
layout(location = 5) out mat3 vTBN;     // occupies locations 5,6,7

void main() {
    vec4 worldPos = pc.model * vec4(inPos, 1.0);
    gl_Position   = cam.viewProj * worldPos;

    vWorldPos = worldPos.xyz;
    vObjPos   = inPos;                                   // unit-sphere object space

    // World normal via the model's normal matrix (handles non-uniform scale).
    mat3 nrm  = transpose(inverse(mat3(pc.model)));
    vWorldNormal = normalize(nrm * inNormal);

    vUV      = inUV;
    vViewDir = normalize(cam.camPos.xyz - worldPos.xyz); // toward camera

    // Analytic UV-sphere tangent (d/d longitude). T points east; B = N x T.
    vec3 N = vWorldNormal;
    vec3 objT = normalize(vec3(-inPos.z, 0.0, inPos.x));  // east tangent (object space)
    if (dot(objT, objT) < 1e-5) objT = vec3(1.0, 0.0, 0.0); // pole fallback
    vec3 T = normalize(mat3(pc.model) * objT);
    T = normalize(T - N * dot(N, T));                     // Gram-Schmidt
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);
}
