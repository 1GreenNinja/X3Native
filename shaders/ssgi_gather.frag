#version 450

// Screen-space global illumination (SSGI) — GATHER pass. CLEAN-ROOM, original.
//
// One-bounce indirect DIFFUSE: for each pixel we reconstruct its VIEW-SPACE
// position + normal from the depth buffer (exactly as ssao.frag does), then march
// a noise-rotated cosine-weighted hemisphere of samples. Each sample is projected
// to screen; if it lands on a nearer surface (i.e. that surface is visible to us
// and roughly in our hemisphere) we treat that surface's LIT HDR radiance as
// incoming indirect light, weighted by the cosine term and a view-space range
// falloff. The accumulated radiance is the one-bounce indirect-diffuse estimate.
//
// Built from public, non-engine references only: the Vulkan 1.3 spec; Real-Time
// Rendering 4th ed. (ambient occlusion + indirect-light chapters); the public
// screen-space directional occlusion / SSGI formulation (Ritschel, Grosch,
// Seidel, "Approximating Dynamic Global Illumination in Image Space", I3D 2009)
// where screen-space occluders also carry surface radiance to produce indirect
// colour bleeding; GPU Gems hemisphere-sampling notes. No game-engine source was
// consulted.
//
// INPUTS (pure depth-reconstruction — NO G-buffer):
//   * set0/binding0: main depth buffer (D32, [0,1], NEAREST) — for view-space
//     position + geometric normal reconstruction.
//   * set0/binding1: the LINEAR HDR scene colour (R16F, LINEAR sampler) — the lit
//     scene from the main pass; this is the radiance we bounce.
//   * set0/binding2: a GI UBO (proj/invProj + tunables + baked kernel/noise).
//
// Half-res RGBA16F output keeps the gather cheap; temporal accumulation +
// bilateral denoise (the following passes) clean up the per-tap noise.
//
// OUTPUT: half-res RGBA16F indirect radiance (rgb) with the gather confidence in
// alpha (used by the apply/denoise weighting). The half-res + cheap march keep
// the cost low; temporal accumulation + a bilateral blur clean up the noise.

layout(set = 0, binding = 0) uniform sampler2D depthTex;   // main depth (D32, [0,1])
layout(set = 0, binding = 1) uniform sampler2D sceneTex;   // linear HDR scene colour

const int kKernelSize = 24;

layout(set = 0, binding = 2) uniform Gi {
    mat4  proj;          // view -> clip (same camera projection as the meshes)
    mat4  invProj;       // clip -> view
    vec4  params0;       // x = radius (view m), y = intensity, z = maxRadiance, w = falloff power
    vec4  params1;       // x = screenW, y = screenH, z = numSamples, w = bias
    vec4  kernel[kKernelSize]; // xyz = cosine-weighted hemisphere sample (tangent space)
    vec4  noise[16];     // 4x4 rotation vectors (xy), tiled across the screen
} ub;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outGi;

vec3 viewPosFromDepth(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 vp  = ub.invProj * ndc;
    return vp.xyz / vp.w;
}

void main() {
    float radius    = ub.params0.x;
    float intensity = ub.params0.y;
    float maxRad    = ub.params0.z;
    float falloffP  = ub.params0.w;
    int   numSamp   = int(ub.params1.z);
    float bias      = ub.params1.w;

    // HALF-RES PASS OVER A FULL-RES DEPTH BUFFER — the identical defect ssao.frag
    // documents at length: a half-res texel centre maps to full-res texel
    // coordinate exactly 2i + 1.0, the SEAM between two depth texels, and the
    // NEAREST depth sampler then picks one or the other on the last bit of the
    // interpolated UV. That is a screen-locked one-texel depth error, and dFdx(P)
    // below turns it into a wrong normal. Nudge half a full-res texel.
    const vec2 dUV = vUV + 0.5 / vec2(ub.params1.x, ub.params1.y);

    float depth = texture(depthTex, dUV).r;
    // Sky / far plane: no surface to gather indirect light for.
    if (depth >= 1.0) { outGi = vec4(0.0); return; }

    vec3 P = viewPosFromDepth(dUV, depth);

    // Reconstruct the view-space geometric normal from screen derivatives of P
    // (no normal buffer — same trick as ssao.frag; stable at half-res on flats).
    vec3 dPdx = dFdx(P);
    vec3 dPdy = dFdy(P);
    vec3 N = normalize(cross(dPdx, dPdy));

    // Per-pixel rotation from the tiled 4x4 noise table to break up banding.
    ivec2 px = ivec2(vUV * vec2(ub.params1.x, ub.params1.y));
    int ni = (px.y & 3) * 4 + (px.x & 3);
    vec3 randomVec = vec3(ub.noise[ni].xy, 0.0);
    vec3 T = normalize(randomVec - N * dot(randomVec, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    vec3  gi      = vec3(0.0);
    float samples = 0.0;
    int   n = clamp(numSamp, 1, kKernelSize);

    for (int i = 0; i < n; ++i) {
        // Cosine-weighted hemisphere sample oriented to the surface, placed at
        // `radius` around P in view space.
        vec3 dir       = TBN * ub.kernel[i].xyz;
        vec3 samplePos = P + dir * radius;

        // Project the sample to screen UV to read the surface there.
        vec4 offset = ub.proj * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        vec2 sUV = offset.xy * 0.5 + 0.5;
        if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) { samples += 1.0; continue; }

        float sDepth = texture(depthTex, sUV).r;
        if (sDepth >= 1.0) { samples += 1.0; continue; }   // hit the sky -> no bounce
        vec3 hitPos = viewPosFromDepth(sUV, sDepth);

        // Vector from our surface to the hit surface.
        vec3  toHit = hitPos - P;
        float dist  = length(toHit);
        if (dist < 1e-4) { samples += 1.0; continue; }
        vec3  L = toHit / dist;

        // Cosine term: the hit must lie in our visible hemisphere.
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= bias) { samples += 1.0; continue; }

        // Range falloff: nearby surfaces contribute more (broad, soft). A surface
        // far beyond `radius` (e.g. a distant wall seen past the sample) is faded
        // so it doesn't smear light across depth discontinuities.
        float range = clamp(1.0 - (dist / radius), 0.0, 1.0);
        range = pow(range, falloffP);

        // Incoming radiance = the lit HDR colour of the hit surface, clamped so a
        // single ultra-bright emissive texel can't blow up the indirect term
        // (firefly suppression).
        vec3 radiance = texture(sceneTex, sUV).rgb;
        radiance = min(radiance, vec3(maxRad));

        gi      += radiance * (ndl * range);
        samples += 1.0;
    }

    gi = gi / max(samples, 1.0) * intensity;
    outGi = vec4(gi, 1.0);
}
