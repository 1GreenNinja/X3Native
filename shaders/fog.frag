#version 450

// DEPTH FOG (atmospheric perspective) — CLEAN-ROOM, original. ART_BIBLE.md §5.
//
// A fullscreen pass blended over the linear-HDR scene target AFTER opaque+glass
// and BEFORE the TAA resolve / bloom chain, so fogged radiance participates in
// temporal history and bloom like any other scene light. The pass is only
// RECORDED when the host opted in (fog density > 0) — when off, no pipeline
// runs and the frame is byte-identical to the pre-fog build (same discipline as
// the r_bloom 0 / sharpen 0 guards in composite.frag).
//
// View-space position is reconstructed from the hardware depth buffer exactly
// like ssao.frag (GLM_FORCE_DEPTH_ZERO_TO_ONE, flipped-Y projection): UV -> NDC
// -> invProj -> view. Extinction is a classic Beer-Lambert exp() on the view
// DISTANCE past a start offset (keeps the viewmodel/arms clean), clamped to a
// maximum opacity so far walls / sky never white out ("no milky wash" law).
// Output = premultiplied-style (fogColor, f) through SRC_ALPHA blending.

layout(set = 0, binding = 0) uniform sampler2D depthTex;   // main depth (D32, [0,1])

layout(push_constant) uniform Push {
    mat4  invProj;        // clip -> view (matches the frame's mesh projection)
    vec4  colorDensity;   // rgb = fog color (linear HDR), w = density (1/m)
    vec4  startMax;       // x = start distance (m), y = max opacity [0..1],
                          // z = height fade Y (view-space, reserved), w = pad
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 viewPosFromDepth(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 vp  = pc.invProj * ndc;
    return vp.xyz / vp.w;
}

void main() {
    float depth = texture(depthTex, vUV).r;
    // Far plane (depth == 1): fog at full path length, still capped by maxOpacity
    // (interiors have no sky; space/sky worlds never opt in — see the host gate).
    vec3  P    = viewPosFromDepth(vUV, min(depth, 0.99999));
    float dist = length(P);
    float d    = max(dist - pc.startMax.x, 0.0);
    float f    = 1.0 - exp(-pc.colorDensity.w * d);
    f = min(f, pc.startMax.y);
    outColor = vec4(pc.colorDensity.rgb, f);
}
