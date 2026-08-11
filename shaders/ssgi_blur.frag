#version 450

// Screen-space global illumination (SSGI) — depth-aware DENOISE blur. CLEAN-ROOM.
//
// Even after temporal accumulation the half-res GI carries low-frequency noise +
// the per-pixel rotation pattern from the gather. A wide depth-aware (bilateral)
// blur over the half-res GI smooths it while a DEPTH weight stops indirect light
// from bleeding across silhouette edges (so bounce light off a near wall doesn't
// smear onto the far geometry behind it). Indirect diffuse is inherently low-
// frequency, so a generous blur is exactly right and hides the half-res origin.
// Reference: the standard bilateral / a-trous edge-aware denoise companion to
// screen-space GI (Real-Time Rendering 4th ed.; public SVGF / a-trous wavelet
// denoise write-ups). No game-engine source consulted.

layout(set = 0, binding = 0) uniform sampler2D giTex;     // accumulated GI (half-res RGBA16F)
layout(set = 0, binding = 1) uniform sampler2D depthTex;  // depth (full-res, NEAREST)

layout(push_constant) uniform Push {
    vec2  giTexel;       // 1 / half-res extent
    float depthSigma;    // depth-difference falloff (clip z)
    float stepScale;     // a-trous tap spacing (1 = adjacent texels)
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outGi;

void main() {
    // HALF-RES PASS OVER A FULL-RES DEPTH BUFFER — see ssao.frag. Every depth
    // fetch at a half-res texel centre lands on the seam between two full-res
    // texels; nudge half a full-res texel (= a quarter of a GI texel) so the
    // NEAREST sampler cannot flip-flop between them.
    const vec2 dNudge = pc.giTexel * 0.25;

    float centerDepth = texture(depthTex, vUV + dNudge).r;
    if (centerDepth >= 1.0) { outGi = texture(giTex, vUV); return; }

    vec4  sum  = vec4(0.0);
    float wsum = 0.0;

    // 5x5 bilateral window with adjustable tap spacing (a-trous style). Depth
    // similarity preserves edges; a small spatial Gaussian softens the result.
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 off = vec2(float(x), float(y)) * pc.giTexel * pc.stepScale;
            vec2 uv  = vUV + off;
            vec4 gi  = texture(giTex, uv);
            float d  = texture(depthTex, uv + dNudge).r;
            float dw = exp(-abs(d - centerDepth) / max(1e-5, pc.depthSigma));
            float sw = exp(-float(x * x + y * y) * 0.25);   // spatial Gaussian
            float w  = dw * sw;
            sum  += gi * w;
            wsum += w;
        }
    }
    outGi = (wsum > 0.0) ? (sum / wsum) : texture(giTex, vUV);
}
