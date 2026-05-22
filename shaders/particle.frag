#version 450

// Camera-facing billboard particle fragment shader (combat-juice GPU particles).
//
// CLEAN-ROOM, original work. The SOFT-PARTICLE depth fade is the standard public
// technique (Real-Time Rendering 4th ed.; the GPU Gems "soft particles" chapter):
// fade the particle's alpha as the scene surface BEHIND it approaches the
// particle's own depth, so the billboard dissolves into geometry instead of
// showing a hard intersection seam.
//
// Shaded in LINEAR HDR, pre-tonemap (the shared ACES curve runs once in
// composite.frag after bloom), so bright additive sparks/muzzle drive the bloom
// chain exactly like the emissive light fixtures + the water sun glint.

layout(set = 0, binding = 0) uniform ParticleUBO {
    mat4 viewProj;
    vec4 camRight;
    vec4 camUp;
    vec4 camPos;
    vec4 params;     // x = 1/screenW, y = 1/screenH, z = near, w = far
} u;

// Scene depth buffer (the SSAO depth pre-pass output), DEPTH_READ_ONLY layout.
// Sampled for the soft-particle fade vs. the opaque scene behind the billboard.
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(location = 0) in vec4 vColor;   // rgb * intensity, a = opacity
layout(location = 1) in vec2 vUv;      // [0,1]

layout(location = 0) out vec4 outColor;

// Reconstruct linear eye-space distance from a [0,1] clip depth (Vulkan reverse-Y
// perspective, GLM_FORCE_DEPTH_ZERO_TO_ONE). Returns +distance in front of camera.
float linearizeDepth(float d, float near, float far) {
    return (near * far) / (far - d * (far - near));
}

void main() {
    // --- Round, soft-edged sprite: radial falloff from the quad center so the
    // billboard reads as a soft puff/spark rather than a hard square. ---
    vec2 c = vUv * 2.0 - 1.0;            // [-1,1]
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;               // outside the unit disc
    float radial = 1.0 - r2;             // 1 at center -> 0 at the rim
    radial = radial * radial;            // tighten the core

    // --- Soft-particle depth fade: compare the opaque scene depth behind this
    // pixel to the particle's own fragment depth. When the scene surface is very
    // close behind the billboard, fade the alpha out so there is no hard seam. ---
    vec2 screenUV = gl_FragCoord.xy * vec2(u.params.x, u.params.y);
    float near = u.params.z, far = u.params.w;
    float sceneD = texture(sceneDepth, screenUV).r;
    float sceneDist = linearizeDepth(sceneD, near, far);
    float fragDist  = linearizeDepth(gl_FragCoord.z, near, far);
    // Fade over a ~0.4 m soft zone (smoke/dust melt into walls/floor).
    float soft = clamp((sceneDist - fragDist) / 0.4, 0.0, 1.0);

    float alpha = vColor.a * radial * soft;
    if (alpha <= 0.0015) discard;        // skip fully-transparent fragments
    // Premultiply the color by the radial falloff so additive sparks glow brighter
    // in the core (HDR -> bloom) while alpha smoke stays soft at the rim.
    outColor = vec4(vColor.rgb * radial, alpha);
}
