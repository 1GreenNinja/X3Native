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
    vec4  colorDensity;   // rgb = NEAR fog color (linear HDR), w = density (1/m)
    vec4  startMax;       // x = start distance (m), y = max opacity [0..1],
                          // z = sky-blend distance (m; 0 = flat single color),
                          // w = pad
    // ---- AERIAL PERSPECTIVE (Phase 0.3). Both default INERT: heightFalloff 0
    // reproduces the flat Beer-Lambert exactly; skyBlendDist 0 keeps one color.
    vec4  upCam;          // xyz = world-up in VIEW space, w = camera world Y
    vec4  skyColor;       // rgb = FAR color (the horizon the scene melts into),
                          // w = height falloff (1/m; 0 = height-independent)
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
    // Exponential HEIGHT fog, closed-form along the camera->P segment:
    // sigma(y) = density * exp(-k*y); integral over the segment divided by its
    // length gives the effective density. k = 0 collapses to the flat original.
    float k  = pc.skyColor.w;
    float sigma = pc.colorDensity.w;
    if (k > 0.0) {
        float camY = pc.upCam.w;
        float dY   = dot(pc.upCam.xyz, P);            // rise over the segment
        float kd   = k * dY;
        float seg  = (abs(kd) > 1e-3) ? (1.0 - exp(-kd)) / kd : 1.0 - 0.5 * kd;
        sigma *= exp(-k * max(camY, 0.0)) * max(seg, 0.0);
    }
    float f = 1.0 - exp(-sigma * d);
    f = min(f, pc.startMax.y);
    // Two-tone aerial grade: near haze melts into the FAR/sky color with
    // distance, so the horizon dissolves into the sky instead of a flat wall.
    vec3 rgb = pc.colorDensity.rgb;
    if (pc.startMax.z > 0.0) {
        float t = clamp(dist / pc.startMax.z, 0.0, 1.0);
        rgb = mix(rgb, pc.skyColor.rgb, t * t * (3.0 - 2.0 * t));
    }
    outColor = vec4(rgb, f);
}
