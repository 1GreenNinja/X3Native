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

struct ObjectData {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissive;
    uint texIndex;
    uint _pad0;
    uint _pad1;
    uint _pad2;
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
    gl_Position = cam.viewProj * o.model * vec4(inPos, 1.0);
}
