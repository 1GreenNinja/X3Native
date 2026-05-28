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
//
// CRITICAL: gl_Position MUST be invariant + computed in EXACTLY the same order
// as mesh.vert (worldPos = model * inPos; gl_Position = viewProj * worldPos).
// The main pass then runs depth-test EQUAL against this Z. Even a 1-ULP
// difference (from FMA reordering or a different bracketing) causes the EQUAL
// test to reject every fragment, leaving the HDR target empty -> blank capture.
// invariant ensures the SPIR-V/driver does not re-order ops in this pre-pass
// differently from the same expression in mesh.vert. See mesh.vert for the
// matching `invariant gl_Position;` + identical computation.

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

// Force position-invariance so the SPIR-V/driver MUST emit the same arithmetic
// sequence as mesh.vert's gl_Position. Without this, the (model * inPos) sub-
// expression can be folded into a fused multiply-add differently in the two
// shaders, and the main pass's depth-EQUAL test rejects every fragment.
invariant gl_Position;

void main() {
    ObjectData o = objBuf.objects[gl_InstanceIndex];
    // MUST match mesh.vert EXACTLY: worldPos = model * inPos; pos = viewProj * worldPos.
    // The `precise` qualifier (and the matching one in mesh.vert) tells the driver
    // not to reorder these matrix multiplies into a different FMA sequence — the
    // main pass's depth-EQUAL/LESS_OR_EQUAL test against this Z then never sees a
    // 1-ULP drift that would reject every fragment (the symptom: a blank --headless
    // --screenshot capture, observed on NVIDIA 1080 Ti / no-RT, raster SSAO/GI).
    precise vec4 worldPos = o.model * vec4(inPos, 1.0);
    precise vec4 clipPos  = cam.viewProj * worldPos;
    gl_Position = clipPos;
}
