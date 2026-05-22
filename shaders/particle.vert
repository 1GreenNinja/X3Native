#version 450

// Camera-facing billboard particle vertex shader (combat-juice GPU particles).
//
// CLEAN-ROOM, original work. Built from public real-time billboard / soft-particle
// references (Real-Time Rendering 4th ed.; the GPU Gems soft-particle / billboard
// chapters). No game-engine source was consulted.
//
// GEOMETRY: ONE unit quad (4 corner verts in [-0.5,0.5], drawn as a triangle
// strip) is expanded per-instance into a world-space billboard that always faces
// the camera. The corner is offset along the camera's RIGHT and UP basis vectors
// (passed in the UBO) scaled by the per-instance half-size, so the quad is screen-
// aligned regardless of view. The simulation runs on the CPU (app/fx.*); each
// frame the live particles are streamed in as the per-instance attributes below.

layout(set = 0, binding = 0) uniform ParticleUBO {
    mat4 viewProj;   // camera view*proj (same matrix the meshes use)
    vec4 camRight;   // xyz = camera right basis (world)
    vec4 camUp;      // xyz = camera up basis (world)
    vec4 camPos;     // xyz = camera world position
    vec4 params;     // x = 1/screenW, y = 1/screenH, z = near, w = far
} u;

// Per-vertex: the unit-quad corner in [-0.5,0.5] (location 0).
layout(location = 0) in vec2 inCorner;
// Per-instance: pos.xyz + half-size (location 1), color.rgba (location 2).
layout(location = 1) in vec4 inPosSize;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec4 vColor;     // rgb * intensity, a = opacity
layout(location = 1) out vec2 vUv;        // [0,1] quad uv (round falloff)

void main() {
    float halfSize = inPosSize.w;
    vec3 center    = inPosSize.xyz;

    // Expand the corner along the screen-aligned camera basis (camera-facing).
    vec3 worldPos = center
                  + u.camRight.xyz * (inCorner.x * 2.0 * halfSize)
                  + u.camUp.xyz    * (inCorner.y * 2.0 * halfSize);

    vColor = inColor;
    vUv    = inCorner + vec2(0.5);   // [-0.5,0.5] -> [0,1]
    gl_Position = u.viewProj * vec4(worldPos, 1.0);
}
