#version 450

// Screen-space global illumination (SSGI) — TEMPORAL accumulation. CLEAN-ROOM.
//
// The raw half-res gather (ssgi_gather.frag) is noisy because it traces only a
// handful of hemisphere samples per pixel per frame. This pass blends the current
// gather with the REPROJECTED previous-frame GI using an exponential moving
// average, dramatically reducing noise over a few frames while staying fully
// dynamic (no baking). Reprojection is CAMERA-ONLY: we reconstruct each pixel's
// world position from the current depth + inverse current viewProj, project it
// with the PREVIOUS frame's viewProj to find where it was last frame, and sample
// the history there. A depth/position disocclusion test rejects history where the
// reprojected sample doesn't correspond to the same surface (e.g. edges revealed
// by camera motion), falling back to the current gather so we don't smear stale
// light. Reference: standard temporal-reprojection / EMA accumulation for
// screen-space effects (Real-Time Rendering 4th ed.; public temporal-AA / SVGF
// reprojection write-ups). No game-engine source consulted.
//
// CAVEAT (honest): camera-only reprojection does not track per-object motion, so
// fast-moving dynamic objects can ghost their indirect light for a few frames.
// A motion-vector buffer would fix this (documented next tier).

layout(set = 0, binding = 0) uniform sampler2D giCur;     // current raw gather (half-res RGBA16F)
layout(set = 0, binding = 1) uniform sampler2D giHist;    // previous accumulated GI (half-res RGBA16F)
layout(set = 0, binding = 2) uniform sampler2D depthTex;  // current depth (full-res, NEAREST)
layout(set = 0, binding = 3) uniform sampler2D depthHist; // previous depth (full-res, NEAREST)

layout(set = 0, binding = 4) uniform GiTemporal {
    mat4  invViewProjCur;   // current clip -> world
    mat4  viewProjPrev;     // world -> previous clip
    vec4  params0;          // x = alpha (history weight), y = depthRejectScale, z = valid(0/1), w = unused
} ub;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outGi;

// Reconstruct WORLD position from a UV + clip-space depth using the current
// inverse viewProj (the meshes used proj[1][1]*=-1 so framebuffer +y is down;
// UV->NDC maps directly).
vec3 worldFromDepth(vec2 uv, float depth, mat4 invVP) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 wp  = invVP * ndc;
    return wp.xyz / wp.w;
}

void main() {
    vec4 cur = texture(giCur, vUV);

    float alpha          = ub.params0.x;
    float depthRejScale  = ub.params0.y;
    bool  histValid      = ub.params0.z > 0.5;

    float depth = texture(depthTex, vUV).r;
    if (depth >= 1.0 || !histValid) { outGi = cur; return; }

    // Current world position -> previous clip -> previous UV.
    vec3 worldPos = worldFromDepth(vUV, depth, ub.invViewProjCur);
    vec4 prevClip = ub.viewProjPrev * vec4(worldPos, 1.0);
    if (prevClip.w <= 0.0) { outGi = cur; return; }
    vec3 prevNdc = prevClip.xyz / prevClip.w;
    vec2 prevUV  = prevNdc.xy * 0.5 + 0.5;

    // Off-screen last frame -> no usable history.
    if (prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0) {
        outGi = cur; return;
    }

    // Disocclusion test: the surface we now see, reprojected, must match where it
    // was. Compare the reconstructed prev-frame world position (from the history
    // depth at prevUV) to our worldPos; reject if they differ too much relative to
    // scene scale (a revealed edge or a different surface).
    float prevDepth = texture(depthHist, prevUV).r;
    if (prevDepth >= 1.0) { outGi = cur; return; }
    vec3 prevWorld = worldFromDepth(prevUV, prevDepth, ub.invViewProjCur); // approx (same proj basis)
    float posErr = length(prevWorld - worldPos);
    // Tolerance scales with distance so far surfaces (coarser depth) aren't over-
    // rejected. depthRejScale tunes the strictness.
    float tol = depthRejScale * (1.0 + length(worldPos) * 0.02);
    if (posErr > tol) { outGi = cur; return; }

    vec4 hist = texture(giHist, prevUV);
    // Exponential moving average: keep `alpha` of history, blend in (1-alpha) of
    // the fresh gather. Higher alpha = smoother but more lag.
    outGi = mix(cur, hist, alpha);
}
