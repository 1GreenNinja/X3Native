#version 450

// Impact-decal vertex shader (bullet holes / scorch marks — combat juice).
//
// CLEAN-ROOM, original work. This is the simple MESH-DECAL approach (an oriented
// quad laid on the hit surface), per the public real-time-decal references
// (Real-Time Rendering 4th ed. decal chapter; the GPU-Gems projected-decal notes).
// Full deferred/projected G-buffer decals are the documented next tier.
//
// GEOMETRY: ONE unit quad (4 corners in [-0.5,0.5], triangle strip) is expanded
// per-instance into a world-space quad that lies ON the hit surface: its face
// normal is the per-instance surface normal, it is sized by halfSize, spun about
// the normal by `angle`, and pushed a hair along the normal to avoid z-fighting
// with the surface it sits on. Drawn alpha-blended, depth-tested, no depth-write.

layout(set = 0, binding = 0) uniform DecalUBO {
    mat4 viewProj;   // camera view*proj (same matrix the meshes use)
    vec4 params;     // reserved
} u;

layout(location = 0) in vec2 inCorner;     // unit-quad corner in [-0.5,0.5]
// Per-instance: center.xyz + halfSize (loc 1), normal.xyz + angle (loc 2), color (loc 3).
layout(location = 1) in vec4 inCenterSize;
layout(location = 2) in vec4 inNormalAngle;
layout(location = 3) in vec4 inColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;

void main() {
    vec3  center   = inCenterSize.xyz;
    float halfSize = inCenterSize.w;
    vec3  N        = normalize(inNormalAngle.xyz);
    float angle    = inNormalAngle.w;

    // Build an orthonormal tangent basis on the surface (T, B) perpendicular to N.
    vec3 up = (abs(N.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T  = normalize(cross(up, N));
    vec3 B  = cross(N, T);
    // Spin the basis about the normal by `angle` for per-decal variety.
    float ca = cos(angle), sa = sin(angle);
    vec3 Tr =  T * ca + B * sa;
    vec3 Br = -T * sa + B * ca;

    // Lay the quad on the surface, pushed slightly out along N (z-fight guard).
    vec3 worldPos = center
                  + Tr * (inCorner.x * 2.0 * halfSize)
                  + Br * (inCorner.y * 2.0 * halfSize)
                  + N  * 0.012;

    vColor = inColor;
    vUv    = inCorner + vec2(0.5);
    gl_Position = u.viewProj * vec4(worldPos, 1.0);
}
