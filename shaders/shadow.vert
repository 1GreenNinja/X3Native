#version 450

// Directional-shadow depth-only vertex shader (perf-stack E).
//
// Renders the scene depth from the SUN's point of view into a 2D depth texture.
// Reuses the SAME per-object SSBO + indirect draw buffer the main mesh pass uses
// (set 0, binding 0), indexed by gl_InstanceIndex (firstInstance + instance), so
// the shadow pass and the color pass draw exactly the same geometry. The only
// transform is lightViewProj * model — there is NO fragment shader (depth-only),
// so this writes gl_Position and nothing else.

struct ObjectData {
    mat4 model;
    vec4 baseColorFactor;
    uint texIndex;
    uint _pad0;
    uint _pad1;
    uint _pad2;
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
    gl_Position = cam.lightViewProj * o.model * vec4(inPos, 1.0);
}
