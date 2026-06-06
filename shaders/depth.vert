#version 450

// Camera depth-only pre-pass vertex shader (SSAO support).
//
// CLEAN-ROOM, original. Renders the scene's CAMERA-space depth into the main
// depth buffer BEFORE the lighting pass so the SSAO pass has a complete depth
// image to reconstruct view-space position + normals from. Reuses the SAME
// per-object SSBO + indirect draw buffer the main mesh pass uses (set 0,
// binding 0), indexed by gl_InstanceIndex, so the pre-pass and the color pass
// rasterize exactly the same geometry to exactly the same depth values (the
// color pass then runs depth-test EQUAL with depth-write off). Depth-only:
// writes gl_Position and nothing else (no fragment shader bound).
//
// This is the camera analogue of shadow.vert (which uses lightViewProj); here
// the transform is cam.viewProj * model so the depth matches the main pass.

// MUST match the std430 stride of mesh.vert's ObjectData (128 B) AND the C++ ObjectData
// (VulkanRenderDevice.cpp, static_assert(sizeof==128)). The pre-pass and color pass share
// the SAME per-object SSBO indexed by gl_InstanceIndex, so a smaller stride here reads the
// WRONG row for every instance index > 0 -> depth.vert writes a different depth than
// mesh.vert -> the color pass's EQUAL depth test discards every instance except SSBO row 0.
// (The PBR slice grew this struct to 128 B in mesh.vert + C++ but missed depth.vert/shadow.vert.)
// depth.vert only uses o.model; the 8 trailing uints exist solely to force stride = 128 B.
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

// Same per-frame Camera UBO the mesh/shadow stages read (set 0, binding 1 here
// because the pre-pass binds the object set as set 0, like the shadow pass).
layout(set = 0, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
} cam;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;   // unused; kept so the VBO layout matches
layout(location = 2) in vec2 inUV;       // unused

void main() {
    ObjectData o = objBuf.objects[gl_InstanceIndex];
    // Group the multiply EXACTLY as mesh.vert (worldPos first, then viewProj) so the
    // floating-point depth is bit-invariant with the color pass and survives EQUAL.
    vec4 worldPos = o.model * vec4(inPos, 1.0);
    gl_Position = cam.viewProj * worldPos;
}
