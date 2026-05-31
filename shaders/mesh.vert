#version 450
#extension GL_EXT_nonuniform_qualifier : require

// GPU-driven mesh vertex shader (Subsystem D + perf-stack E shadows).
//
// No per-draw push constants. Per-object data lives in a per-frame SSBO indexed
// by gl_InstanceIndex (the indirect draw's firstInstance + instance), and the
// camera viewProj is a single frame UBO. The fragment shader samples a bindless
// texture array by the per-object texIndex carried through here. For shadow
// mapping (E) the world-space position is forwarded so the fragment stage can
// project it into the sun's light space and PCF-sample the shadow map.

struct ObjectData {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissive;        // rgb = linear emissive color, a = strength (HDR source)
    uint texIndex;
    uint flags;           // bit0 = TERRAIN splat, bit1 = GLASS (was _pad0/terrainFlag)
    uint terrainPack1;    // grass<<16 | rock detail bindless indices (was _pad1)
    uint terrainPack2;    // snow<<16  | sand detail bindless indices (was _pad2)
    uint normalTexIndex;  // PBR slice 1 bindless idx (0 = none, slice 2 reads it)
    uint mrTexIndex;
    uint _pad3, _pad4;
    vec4 glassParams;     // GLASS only: x=refraction, y=roughness, z=specular, w=metallic
    vec4 glassTint;       // GLASS only: rgb=tint, w=ior
    vec4 glassExtra;      // GLASS only: x=reflectance, yzw=transmittanceColor
};

// Per-object flag bits (match VulkanRenderDevice.cpp kFlag*).
const uint FLAG_TERRAIN = 1u;
const uint FLAG_GLASS   = 2u;

layout(std430, set = 1, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

// Per-frame UBO (set1/binding1). The vertex stage only needs viewProj, but the
// block layout MUST match the fragment shader's (same buffer): camera viewProj +
// sun lightViewProj, then the point-light header + array. See mesh.frag / FrameUBO.
struct PointLight {
    vec4 posRange;   // xyz = world position, w = range
    vec4 colorPad;   // rgb = color * intensity, a = unused
};
const int kMaxPointLights = 64;
layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 ambientCount;
    PointLight lights[kMaxPointLights];
} cam;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTexIndex;
layout(location = 3) flat out vec4 vFactor;
layout(location = 4) out vec3 vWorldPos;
layout(location = 5) flat out vec4 vEmissive;   // rgb = color, a = strength
// Per-object flags forwarded to the fragment stage (TERRAIN splat / GLASS).
layout(location = 6) flat out uint vFlags;
layout(location = 7) flat out uvec2 vTerrainPack; // x = grass<<16|rock, y = snow<<16|sand
// GLASS material forwarded to glass.frag (M2-M4): refraction/roughness/specular + tint.
layout(location = 8) flat out vec4 vGlassParams;  // x = refraction, y = roughness, z = specular, w = metallic
layout(location = 9) flat out vec4 vGlassTint;    // rgb = tint (baseColor), w = ior
layout(location = 10) flat out vec4 vGlassExtra;  // x = reflectance, yzw = transmittanceColor

// POSITION INVARIANCE: the depth pre-pass (depth.vert) writes the camera depth
// before this pass, and the main pass's pipeline runs depth-test EQUAL. For the
// equality to ever hold, both shaders MUST compute gl_Position with the same
// arithmetic sequence at the same precision. `invariant` blocks the compiler/
// driver from reordering ops in this expression independently of the matching
// `invariant gl_Position;` in depth.vert. Without it, FMA fusion on certain
// drivers (1080 Ti / NVIDIA) produces Z values that differ by 1 ULP -> the
// EQUAL test rejects every fragment -> HDR target stays cleared -> blank PNG.
invariant gl_Position;

void main() {
    ObjectData o = objBuf.objects[gl_InstanceIndex];
    // `precise` matches the same qualifier in depth.vert so both shaders compute
    // gl_Position with the SAME arithmetic sequence at the same precision (no FMA
    // reordering), and the main pass's depth-EQUAL test against the pre-pass Z
    // never spuriously rejects fragments. See depth.vert.
    precise vec4 worldPos = o.model * vec4(inPos, 1.0);
    precise vec4 clipPos  = cam.viewProj * worldPos;
    gl_Position = clipPos;
    vNormal = mat3(o.model) * inNormal;
    vUV = inUV;
    vTexIndex = o.texIndex;
    vFactor = o.baseColorFactor;
    vWorldPos = worldPos.xyz;
    vEmissive = o.emissive;
    vFlags = o.flags;
    vTerrainPack = uvec2(o.terrainPack1, o.terrainPack2);
    vGlassParams = o.glassParams;
    vGlassTint = o.glassTint;
    vGlassExtra = o.glassExtra;
}
