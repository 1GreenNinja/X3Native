#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Reflection-PROBE variant of mesh.vert. Identical per-object SSBO + outputs, but
// the view-projection comes from a PUSH CONSTANT (one mat4 per cube face) instead
// of the per-frame Camera UBO. This lets the interior reflection-probe bake render
// the scene into all 6 env-cube faces inside ONE command submit (a single mapped
// camera UBO cannot hold 6 different per-face matrices at execute time). Pairs with
// the UNCHANGED mesh.frag, so probe-rendered surfaces are lit exactly like the main
// pass (direct sun/points + ambient/IBL). See regenIblFromSky() probe pass.

struct ObjectData {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissive;        // rgb = linear emissive color, a = strength (HDR source)
    uint texIndex;
    uint flags;           // bit0 = TERRAIN, bit1 = GLASS (was terrainFlag)
    uint terrainPack1;
    uint terrainPack2;
    uint normalTexIndex;
    uint mrTexIndex;
    uint emissiveTexIndex;
    uint detailPacked;    // HDRP micro-detail (_pad4): (uvScale*64<<20)|idx
    vec4 glassParams;     // GLASS only: x = refraction, y = roughness, z = specular
    vec4 glassTint;       // GLASS only: rgb = tint color (matches C++ 160B ObjectData)
};

layout(std430, set = 1, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

// Per-face view-projection (probe point + 90deg FOV looking down each cube axis).
layout(push_constant) uniform ProbePush {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTexIndex;
layout(location = 3) flat out vec4 vFactor;
layout(location = 4) out vec3 vWorldPos;
layout(location = 5) flat out vec4 vEmissive;
layout(location = 6) flat out uint vFlags;       // bit0 = TERRAIN, bit1 = GLASS
layout(location = 7) flat out uvec2 vTerrainPack;
layout(location = 8) flat out uint vNormalTexIndex;
layout(location = 9) flat out uint vMrTexIndex;
layout(location = 10) flat out uint vEmissiveTexIndex;
layout(location = 11) flat out uint vDetailPacked;     // HDRP micro-detail (_pad4): (uvScale*64<<20)|idx

void main() {
    ObjectData o = objBuf.objects[gl_InstanceIndex];
    vec4 worldPos = o.model * vec4(inPos, 1.0);
    gl_Position = pc.viewProj * worldPos;
    vNormal = mat3(o.model) * inNormal;
    vUV = inUV;
    vTexIndex = o.texIndex;
    vFactor = o.baseColorFactor;
    vWorldPos = worldPos.xyz;
    vEmissive = o.emissive;
    vFlags = o.flags;
    vTerrainPack = uvec2(o.terrainPack1, o.terrainPack2);
    vNormalTexIndex = o.normalTexIndex;
    vMrTexIndex = o.mrTexIndex;
    vEmissiveTexIndex = o.emissiveTexIndex;
    vDetailPacked = o.detailPacked;   // HDRP micro-detail map (packed idx + uvScale)
}
