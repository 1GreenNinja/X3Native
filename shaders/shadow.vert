#version 450

// Directional-shadow depth-only vertex shader (perf-stack E).
//
// Renders the scene depth from the SUN's point of view into a 2D depth texture.
// Reuses the SAME per-object SSBO + indirect draw buffer the main mesh pass uses
// (set 0, binding 0), indexed by gl_InstanceIndex (firstInstance + instance), so
// the shadow pass and the color pass draw exactly the same geometry. The only
// transform is lightViewProj * model — there is NO fragment shader (depth-only),
// so this writes gl_Position and nothing else.

// MUST match the std430 stride of mesh.vert's ObjectData (128 B) + the C++ ObjectData
// (VulkanRenderDevice.cpp static_assert(sizeof==128)) — this pass shares the SAME per-object
// SSBO indexed by gl_InstanceIndex. A smaller stride reads the WRONG row for every instance
// index > 0 (was 96 B: missing `emissive` AND the PBR-slice trailing uints). shadow.vert only
// uses o.model; the rest exist solely to force stride = 128 B.
struct ObjectData {
    mat4 model;          // 64
    vec4 baseColorFactor;// 16
    vec4 emissive;       // 16
    uint texIndex;       // -- 8 uints = 32 B -> total 128 B
    uint _pad0;
    uint _pad1;
    uint _pad2;
    uint _pad3;
    uint _pad4;
    uint _pad5;
    uint _pad6;
};

layout(std430, set = 0, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

// Camera UBO carries both the camera viewProj and the sun's lightViewProj; the
// shadow pass only reads lightViewProj. Same struct as mesh.vert/mesh.frag so a
// single per-frame UBO feeds every stage.
layout(set = 0, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
} cam;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;   // unused; kept so the VBO layout matches
layout(location = 2) in vec2 inUV;       // unused

void main() {
    ObjectData o = objBuf.objects[gl_InstanceIndex];
    vec4 worldPos = o.model * vec4(inPos, 1.0);
    gl_Position = cam.lightViewProj * worldPos;
}
