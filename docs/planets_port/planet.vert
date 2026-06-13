#version 450
// =============================================================================
//  planet.vert  —  one shared vertex shader for every FORGE3D planet type.
//
//  Drives a UV-SPHERE mesh authored in OBJECT space as a UNIT sphere (radius 1,
//  centered at the origin). The model matrix scales/positions/rotates it in the
//  world. Object-space position on a unit sphere == the object-space normal
//  direction, which is exactly what the ASE triplanar samplers need.
//
//  Outputs (consumed by planet_<type>.frag):
//    vWorldPos    world-space position
//    vWorldNormal world-space normal (normalized)
//    vObjPos      OBJECT-space position (unit sphere; == object normal dir)
//    vUV          lat-long UV (longitude, latitude) — for clouds/city/ramps
//    vViewDir     world-space direction TOWARD the camera
//    vTBN         tangent->world basis (columns T,B,N) for tangent normal maps
//
//  This mirrors the engine's existing mesh.vert layout (per-frame Camera UBO at
//  set=1,binding=1 with viewProj + camPos; per-object SSBO at set=1,binding=0).
//  For a DEDICATED planet pipeline (recommended) the per-object SSBO row can be a
//  simpler PlanetObject {mat4 model; uint paramSlot;} — but keeping the existing
//  ObjectData layout lets a planet ride the standard GPU-driven draw path too.
// =============================================================================

// --- Per-object data (same SSBO row the engine's mesh path uses; 128B stride) ---
struct ObjectData {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissive;
    uint texIndex;        // planet param-block / first texture slot (engine-wired)
    uint terrainFlag;
    uint terrainPack1;
    uint terrainPack2;
    uint normalTexIndex;
    uint mrTexIndex;
    uint emissiveTexIndex;
    uint _pad4;
};
layout(std430, set = 1, binding = 0) readonly buffer Objects { ObjectData objects[]; } objBuf;

struct PointLight { vec4 posRange; vec4 colorPad; };
const int kMaxPointLights = 64;
layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 ambientCount;
    PointLight lights[kMaxPointLights];
    vec4 camPos;          // xyz = camera world position
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
    ObjectData o = objBuf.objects[gl_InstanceIndex];

    vec4 worldPos = o.model * vec4(inPos, 1.0);
    gl_Position   = cam.viewProj * worldPos;

    vWorldPos = worldPos.xyz;
    vObjPos   = inPos;                                   // unit-sphere object space

    // World normal via the model's normal matrix (handles non-uniform scale).
    mat3 nrm  = transpose(inverse(mat3(o.model)));
    vWorldNormal = normalize(nrm * inNormal);

    vUV      = inUV;
    vViewDir = normalize(cam.camPos.xyz - worldPos.xyz); // toward camera

    // Tangent frame for tangent-space normal maps. Derive an analytic UV-sphere
    // tangent (d/d longitude) so the TBN is consistent across the planet body.
    // T points east along the sphere; B = N x T.
    vec3 N = vWorldNormal;
    vec3 objT = normalize(vec3(-inPos.z, 0.0, inPos.x));  // east tangent (object space)
    if (dot(objT, objT) < 1e-5) objT = vec3(1.0, 0.0, 0.0); // pole fallback
    vec3 T = normalize(mat3(o.model) * objT);
    T = normalize(T - N * dot(N, T));                     // Gram-Schmidt
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);
}
