#version 450

// Screen-space global illumination (SSGI) — APPLY pass. CLEAN-ROOM, original.
//
// Adds the denoised indirect-diffuse GI back into the LINEAR HDR scene target
// (additive blend, ONE/ONE), BEFORE bloom + tonemap, so the bounce light feeds
// the HDR pipeline exactly like direct light. The half-res GI is up-sampled with
// a small depth-aware (bilateral) filter so it doesn't bleed indirect light
// across silhouette edges when magnified. The contribution is modulated by the
// SSAO ambient occlusion (corners/crevices that occlude direct ambient also
// occlude the gathered bounce) and scaled by the global GI strength.
//
// IMPORTANT: this LIFTS the bounce/ambient term only. Direct sun + point lights
// were already written full-strength by the main pass; GI adds colour bleeding +
// believable fill into shadowed areas instead of flat constant ambient. Sky /
// far-plane pixels (depth==1) receive no GI.
//
// Reference: indirect-light compositing in linear HDR (Real-Time Rendering 4th
// ed.); bilateral up-sampling of half-res buffers (public SSAO/SSGI articles).
// No game-engine source consulted.

layout(set = 0, binding = 0) uniform sampler2D giTex;     // denoised GI (half-res RGBA16F, LINEAR)
layout(set = 0, binding = 1) uniform sampler2D depthTex;  // depth (full-res, NEAREST)
layout(set = 0, binding = 2) uniform sampler2D aoTex;     // SSAO blurred AO (half-res R8, LINEAR)

layout(push_constant) uniform Push {
    vec2  giTexel;       // 1 / half-res extent
    float strength;      // global GI apply strength
    float aoAmount;      // 0 = ignore AO, 1 = fully modulate GI by AO
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    float centerDepth = texture(depthTex, vUV).r;
    // Sky / far plane: no indirect contribution (additive 0).
    if (centerDepth >= 1.0) { outColor = vec4(0.0); return; }

    // Depth-aware bilateral up-sample of the half-res GI: 4 nearest half-res taps
    // weighted by depth similarity to the full-res centre, avoiding edge bleed.
    vec2 base = vUV;
    vec3 gi   = vec3(0.0);
    float wsum = 0.0;
    const ivec2 offs[4] = ivec2[4](ivec2(0,0), ivec2(1,0), ivec2(0,1), ivec2(1,1));
    for (int i = 0; i < 4; ++i) {
        vec2 uv = base + vec2(offs[i]) * pc.giTexel;
        float d = texture(depthTex, uv).r;
        float w = exp(-abs(d - centerDepth) / 0.0015);
        gi   += texture(giTex, uv).rgb * w;
        wsum += w;
    }
    gi = (wsum > 0.0) ? (gi / wsum) : texture(giTex, base).rgb;

    // Modulate by SSAO so occluded geometry doesn't receive full bounce. aoAmount
    // lerps how strongly AO gates the GI (AO is 1=open, 0=occluded).
    float ao = texture(aoTex, vUV).r;
    float aoMod = mix(1.0, ao, clamp(pc.aoAmount, 0.0, 1.0));

    outColor = vec4(gi * pc.strength * aoMod, 0.0);
}
