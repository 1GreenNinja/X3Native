#version 450
// GPU-compute debris draw — instanced SHARD MESHES (Subsystem K, tier T2).
//
// CLEAN-ROOM, original work. Built ONLY from the K spec (§7 indirect/instanced
// debris draw), the Vulkan 1.3 spec, and public real-time references. No id Tech /
// RBDOOM source consulted.
//
// GIB-MESH PASS (fix/gib-meshes): debris used to be ONE shared unit cube, which
// made gibs read as literal red boxes tumbling through the air. The draw is still
// ONE instanced call over the pool capacity, but the per-vertex geometry is now
// fetched from a small SSBO holding a set of kShardCount distinct low-poly
// IRREGULAR shard meshes (authored procedurally once at device init). Each
// instance picks its shard by an integer hash of its pool slot (so a single burst
// spans several distinct meshes) and applies a slight per-instance darkness
// variation so a pile of gibs doesn't read as one flat color. DEAD/parked slots
// still collapse to a degenerate point (no per-frame compaction needed).

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

// Shard mesh set: kShardCount shards, each padded to kShardVerts vertices
// (triangle list, flat normals; padding repeats the last vertex -> degenerate,
// zero-area triangles). Two vec4 rows per vertex: [0]=position, [1]=normal.
// MUST match kGpuDebrisShardCount / kDebrisShardVertsMax and the generation in
// engine/rhi/vk/vk_resources.cpp createDebris().
layout(std430, set = 0, binding = 2) readonly buffer Shards { vec4 sv[]; };
const uint kShardCount = 6u;
const uint kShardVerts = 36u;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec4 vColor;

// Rotate a vector by a quaternion (x,y,z,w).
vec3 qrot(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// Integer hash of the pool slot. MUST match x3::rhi::gpuDebrisShardHash in
// engine/rhi/IRenderDevice.h (the gib self-test asserts mesh variety through it).
uint slotHash(uint s) {
    uint h = s * 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return h;
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
    uint h = slotHash(uint(gl_InstanceIndex));
    uint shard = h % kShardCount;
    uint vi = (shard * kShardVerts + uint(gl_VertexIndex)) * 2u;
    vec3 corner = sv[vi + 0u].xyz;   // shard-local position, |coord| <= 0.5 (unit box)
    vec3 nrm    = sv[vi + 1u].xyz;   // flat per-triangle normal
    // Slight per-instance darkness variation (fresh vs drying gore) from more
    // mixed hash bits; keeps a pile of chunks from reading as one flat color.
    uint h2 = h * 0xC2B2AE35u; h2 ^= h2 >> 16;
    float shade = 0.70 + 0.35 * float(h2 & 0xFFFFu) / 65535.0;
    float halfExt = f.velScale.w;
    vec3 local = corner * (halfExt * 2.0);         // unit shard (half 0.5) -> 2*halfExt
    vec3 world = f.posLife.xyz + qrot(f.rot, local);
    vNormal = normalize(qrot(f.rot, nrm));
    vColor  = vec4(u.color.rgb * shade, u.color.a);
    gl_Position = u.viewProj * vec4(world, 1.0);
}
