#version 450
// Ray-traced AO — APPLY pass. CLEAN-ROOM, original.
//
// Multiplies the LINEAR HDR scene by the ray-traced ambient-occlusion factor
// (depth-aware up-sampled from the half-res RT AO image) BEFORE bloom + tonemap.
// The pipeline is configured with a MULTIPLY blend (dstColor * srcColor), so this
// shader outputs the per-pixel DARKENING factor and the blender multiplies it into
// the existing HDR target without this pass reading that target back (no feedback
// hazard, one cheap full-screen pass).
//
// AO is 1 = fully open (no darkening), 0 = fully occluded. `strength` lerps the
// applied effect so the scene is never over-crushed; sky / far-plane pixels get
// factor 1 (untouched). Reference: AO compositing in linear HDR (Real-Time
// Rendering 4th ed.); depth-aware bilateral up-sampling of half-res buffers
// (public SSAO articles). No game-engine source consulted.

layout(set = 0, binding = 0) uniform sampler2D aoTex;     // half-res RT AO (R8, LINEAR)
layout(set = 0, binding = 1) uniform sampler2D depthTex;  // full-res depth (NEAREST)

layout(push_constant) uniform Push {
    vec2  aoTexel;       // 1 / half-res extent
    float strength;      // 0 = no darkening, 1 = full AO
    float pad0;
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    float centerDepth = texture(depthTex, vUV).r;
    // Sky / far plane: leave untouched (multiply by 1).
    if (centerDepth >= 1.0) { outColor = vec4(1.0); return; }

    // Depth-aware bilateral up-sample of the half-res AO: 4 nearest half-res taps
    // weighted by depth similarity, so AO doesn't bleed across silhouette edges.
    vec3 ao = vec3(0.0);
    float wsum = 0.0;
    const ivec2 offs[4] = ivec2[4](ivec2(0,0), ivec2(1,0), ivec2(0,1), ivec2(1,1));
    for (int i = 0; i < 4; ++i) {
        vec2 uv = vUV + vec2(offs[i]) * pc.aoTexel;
        float d = texture(depthTex, uv).r;
        float w = exp(-abs(d - centerDepth) / 0.0015);
        ao   += vec3(texture(aoTex, uv).r) * w;
        wsum += w;
    }
    float a = (wsum > 0.0) ? (ao.r / wsum) : texture(aoTex, vUV).r;
    a = clamp(a, 0.0, 1.0);

    // Lerp toward the AO factor by strength, but FLOOR the darkening so AO only
    // attenuates the scene toward a minimum (it must never crush the lit result to
    // black). AO is contact/ambient occlusion — it darkens crevices, it does not
    // remove direct light. kMinFactor keeps fully-occluded pixels at ~35% rather
    // than 0, so direct-lit surfaces stay readable (a true ambient-only term would
    // need the lighting split; this is the correct conservative full-frame apply).
    const float kMinFactor = 0.35;
    float aoFactor = mix(kMinFactor, 1.0, a);    // a=1 -> 1.0 (open), a=0 -> 0.35
    float f = mix(1.0, aoFactor, clamp(pc.strength, 0.0, 1.0));
    outColor = vec4(vec3(f), 1.0);
}
