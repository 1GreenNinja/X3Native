#version 450

// SSAO depth-aware (bilateral) blur — CLEAN-ROOM, original.
//
// The raw SSAO from ssao.frag carries the 4x4 noise-rotation tiling pattern and
// per-tap variance. A small box/bilateral blur over the half-res AO removes the
// tiling while a DEPTH weight stops occlusion bleeding across silhouette edges
// (so a foreground prop's contact shadow doesn't smear onto the far wall behind
// it). The blur runs at the same half resolution as the AO; the main pass then
// up-samples it with a linear sampler. Reference: the standard bilateral-blur
// companion to hemisphere SSAO (Real-Time Rendering 4th ed.; Bavoil/Sainz HBAO
// notes on edge-aware AO filtering). No game-engine source consulted.

layout(set = 0, binding = 0) uniform sampler2D aoTex;     // raw AO (R8, half-res)
layout(set = 0, binding = 1) uniform sampler2D depthTex;  // main depth (D32, full-res)

layout(push_constant) uniform Push {
    vec2  aoTexel;       // 1 / half-res extent (AO texel size)
    float depthSigma;    // depth-difference falloff (view-space-ish, in clip z)
    float pad0;
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out float outAO;

void main() {
    float centerDepth = texture(depthTex, vUV).r;
    float sum = 0.0;
    float wsum = 0.0;

    // 4x4 tap window (covers the 4x4 noise tile exactly), bilaterally weighted by
    // depth similarity so edges are preserved.
    for (int y = -2; y <= 1; ++y) {
        for (int x = -2; x <= 1; ++x) {
            vec2 off = vec2(float(x), float(y)) * pc.aoTexel;
            vec2 uv = vUV + off;
            float ao = texture(aoTex, uv).r;
            float d  = texture(depthTex, uv).r;
            // Depth weight: nearby (in depth) taps count fully, distant ones fade.
            float dw = exp(-abs(d - centerDepth) / max(1e-5, pc.depthSigma));
            sum  += ao * dw;
            wsum += dw;
        }
    }
    outAO = (wsum > 0.0) ? (sum / wsum) : texture(aoTex, vUV).r;
}
