#version 450

// Screen-space ambient occlusion (SSAO) — CLEAN-ROOM, original.
//
// Built from public, non-engine references only: the Vulkan 1.3 spec, Real-Time
// Rendering 4th ed. (ambient occlusion chapter), Crytek's original SSAO (Kajalin,
// "Screen Space Ambient Occlusion", ShaderX7) and the widely-published hemisphere-
// kernel formulation (sample a cosine-weighted hemisphere oriented to the surface
// normal, project each sample to screen, compare its view-space depth to the
// stored depth, accumulate occlusion with a range check). No game-engine source
// was consulted.
//
// INPUTS (pure depth-reconstruction — NO G-buffer):
//   * The main depth buffer (set0/binding0): a HARDWARE depth image sampled as a
//     normal sampler2D (D32_SFLOAT, value = Vulkan clip-space z in [0,1]).
//   * An SSAO UBO (set0/binding1): the projection + inverse-projection matrices
//     (depth -> view-space position + view-space position -> screen UV), the
//     tunable radius / bias / intensity / power, the screen extent, and a baked
//     hemisphere kernel + 4x4 rotation-noise table.
//
// We reconstruct each pixel's VIEW-SPACE position from its depth, derive the
// view-space normal from the screen-space derivatives of that position (so no
// normal buffer is needed), rotate the kernel by a per-pixel noise vector to
// break up banding, and accumulate occlusion. Output is single-channel AO in
// R8 (1 = fully unoccluded, 0 = fully occluded) at HALF resolution; a separate
// depth-aware blur pass (ssao_blur.frag) removes the 4x4 noise tiling.

layout(set = 0, binding = 0) uniform sampler2D depthTex;   // main depth (D32, [0,1])

const int kKernelSize = 32;

layout(set = 0, binding = 1) uniform Ssao {
    mat4  proj;          // view -> clip (the camera projection, reverse-Y as built)
    mat4  invProj;       // clip -> view (inverse of proj)
    vec4  params0;       // x = radius (view m), y = bias, z = intensity, w = power
    vec4  params1;       // x = screenW, y = screenH, z = noiseScale.x, w = noiseScale.y
    vec4  kernel[kKernelSize]; // xyz = hemisphere sample (tangent space), w unused
    vec4  noise[16];     // 4x4 rotation vectors (xy), tiled across the screen
} ub;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out float outAO;

// Reconstruct VIEW-SPACE position from a UV + a stored clip-space depth. The
// projection used GLM_FORCE_DEPTH_ZERO_TO_ONE (z in [0,1]) and a flipped Y
// (proj[1][1] *= -1) so framebuffer +y is down; mapping UV->NDC accordingly and
// running invProj recovers the camera-space (view) position.
vec3 viewPosFromDepth(vec2 uv, float depth) {
    // UV [0,1] (framebuffer, +y down) -> NDC. Vulkan NDC x,y in [-1,1], z in [0,1].
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 vp  = ub.invProj * ndc;
    return vp.xyz / vp.w;
}

void main() {
    float radius    = ub.params0.x;
    float bias      = ub.params0.y;
    float intensity = ub.params0.z;
    float power     = ub.params0.w;

    float depth = texture(depthTex, vUV).r;

    // Far plane / sky (depth == 1 with reverse-Z-less [0,1] clip == far): no AO.
    if (depth >= 1.0) { outAO = 1.0; return; }

    vec3 P = viewPosFromDepth(vUV, depth);

    // Reconstruct the view-space normal from screen-space derivatives of P. Using
    // the cross product of the partials gives a per-pixel geometric normal without
    // a normal buffer. dFdx/dFdy operate per 2x2 quad; the SSAO target is half-res
    // so this is stable for flat surfaces and degrades gracefully at edges.
    vec3 dPdx = dFdx(P);
    vec3 dPdy = dFdy(P);
    vec3 N = normalize(cross(dPdx, dPdy));

    // Per-pixel rotation vector from the tiled 4x4 noise table (indexed by the
    // full-res pixel position so the pattern is screen-stable, then de-tiled by
    // the blur). Build a TBN basis (Gram-Schmidt) to orient the hemisphere kernel.
    ivec2 px = ivec2(vUV * vec2(ub.params1.x, ub.params1.y));
    int ni = (px.y & 3) * 4 + (px.x & 3);
    vec3 randomVec = vec3(ub.noise[ni].xy, 0.0);
    vec3 T = normalize(randomVec - N * dot(randomVec, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occlusion = 0.0;
    for (int i = 0; i < kKernelSize; ++i) {
        // Orient the tangent-space hemisphere sample to the surface, place it in
        // view space at `radius` around P.
        vec3 samplePos = P + (TBN * ub.kernel[i].xyz) * radius;

        // Project the sample to screen UV to look up the stored depth there.
        vec4 offset = ub.proj * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        vec2 sUV = offset.xy * 0.5 + 0.5;       // NDC -> UV (matches framebuffer)

        // Off-screen taps contribute no occlusion.
        if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) continue;

        float sDepth = texture(depthTex, sUV).r;
        if (sDepth >= 1.0) continue;            // sample hit the sky/far plane
        vec3 sampleSurface = viewPosFromDepth(sUV, sDepth);

        // Occluded when the actual surface at the sample's screen position is
        // CLOSER to the camera than the sample point (i.e. the sample is behind
        // geometry). In view space the camera looks down -Z, so "closer" == larger
        // z (less negative). A smooth range check fades occlusion from far-away
        // geometry so a distant wall doesn't darken a foreground pixel.
        float rangeCheck = smoothstep(0.0, 1.0, radius / max(0.0001, abs(P.z - sampleSurface.z)));
        occlusion += (sampleSurface.z >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(kKernelSize)) * intensity;
    occlusion = clamp(occlusion, 0.0, 1.0);
    outAO = pow(occlusion, power);
}
