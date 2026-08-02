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
    uint texIndex;       // -- 8 uints = 32 B -> 128 B, + 2 vec4 glass = 160 B
    uint _pad0;
    uint _pad1;
    uint _pad2;
    uint _pad3;
    uint _pad4;
    uint _pad5;
    uint _pad6;
    vec4 glassParams;    // pad to match C++ 160 B ObjectData std430 stride
    vec4 glassTint;      // (unused here — shadow pass only needs `model`)
};

layout(std430, set = 0, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

// D15 GPU cull: same single indirection as mesh.vert (identity when cull off).
// REQUIRED here too — when cull.comp compacts survivors, gl_InstanceIndex no
// longer addresses object rows directly, and this pass replays the SAME indirect
// commands the color pass consumes.
layout(std430, set = 0, binding = 2) readonly buffer VisibleIdx {
    uint idx[];
} visBuf;

// Camera UBO carries both the camera viewProj and the sun's lightViewProj. The
// shadow pass no longer reads either (see the push constant below), but the
// block stays so this shader remains layout-compatible with set 0 as declared by
// mesh.vert/mesh.frag — one per-frame UBO feeds every stage.
layout(set = 0, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
} cam;

// CASCADED SHADOW MAPS: the light matrix of the cascade CURRENTLY being
// rasterized, pushed once per cascade by recordShadowPassBody. A push constant
// rather than a UBO array because the per-frame Camera UBO's std140 layout is
// mirrored in ~20 GLSL files and widening it would touch every one of them.
// With r_csm 0 exactly ONE cascade is drawn and the pushed matrix IS the legacy
// cam.lightViewProj, so the rasterized depth is unchanged.
layout(push_constant) uniform CascadePush {
    mat4 lightViewProj;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;   // unused; kept so the VBO layout matches
layout(location = 2) in vec2 inUV;       // unused

void main() {
    ObjectData o = objBuf.objects[visBuf.idx[gl_InstanceIndex]];
    vec4 worldPos = o.model * vec4(inPos, 1.0);
    gl_Position = pc.lightViewProj * worldPos;
}
