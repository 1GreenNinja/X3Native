#version 450
// GPU-compute debris draw — instanced unit cube (Subsystem K, tier T2).
//
// CLEAN-ROOM, original work. Built ONLY from the K spec (§7 indirect/instanced
// debris draw), the Vulkan 1.3 spec, and public real-time references. No id Tech /
// RBDOOM source consulted.
//
// Reuses the GPU-driven instanced pattern already in the renderer: ONE draw of a
// shared unit cube with instanceCount == the debris pool capacity; each instance
// reads its OWN fragment row from the same pool SSBO the compute shader integrates
// (via gl_InstanceIndex), builds a model matrix from the position + orientation +
// half-extent, and DEAD/parked slots are collapsed to a degenerate point so they
// produce no visible geometry (no per-frame compaction needed).

layout(set = 0, binding = 0) uniform DebrisDrawUBO {
    mat4 viewProj;
    vec4 color;      // rgb tint, a = opacity
} u;

struct Fragment {
    vec4 posLife;   // xyz pos, w life
    vec4 velScale;  // xyz vel, w half-extent
    vec4 spinState; // xyz ang vel, w state+sleepCtr
    vec4 rot;       // quaternion (x,y,z,w)
};
layout(std430, set = 0, binding = 1) readonly buffer Pool { Fragment frags[]; };

// Unit-cube corner geometry (per-vertex), half-extent 0.5; expanded by the
// fragment's half-extent + orientation below.
layout(location = 0) in vec3 inCorner;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec4 vColor;

// Rotate a vector by a quaternion (x,y,z,w).
vec3 qrot(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    Fragment f = frags[gl_InstanceIndex];
    float state = floor(f.spinState.w + 0.5);
    if (state < 0.5) {
        // DEAD slot: emit a degenerate vertex (clip-space NaN-free off-screen point).
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vNormal = vec3(0.0, 1.0, 0.0);
        vColor = vec4(0.0);
        return;
    }
    float half = f.velScale.w;
    vec3 local = inCorner * (half * 2.0);          // unit cube (half 0.5) -> 2*half
    vec3 world = f.posLife.xyz + qrot(f.rot, local);
    vNormal = normalize(qrot(f.rot, inNormal));
    vColor  = u.color;
    gl_Position = u.viewProj * vec4(world, 1.0);
}
