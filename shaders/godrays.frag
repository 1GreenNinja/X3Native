#version 450

// VOLUMETRIC GOD-RAYS / LIGHT SHAFTS — screen-space radial scatter (mode 1).
// CLEAN-ROOM, original. The classic crepuscular-rays / radial-blur-of-an-
// occlusion-buffer technique (Mitchell, GPU Gems 3 ch.13 family): from each
// pixel we march a fixed number of samples back toward the sun's screen
// position, accumulating an OCCLUSION-MASKED brightness so the shafts emanate
// from the actual bright source edges (the sun disc, an engine bell, a window).
//
// The "occluder mask" is built inline from the HDR scene + depth: a texel
// contributes to the shaft only where it is BRIGHT (toward-sun radiance above a
// knee) AND effectively at the far plane / sky distance (so solid geometry in
// front of the light OCCLUDES the shaft, carving the stabbing-past-the-ship
// look). Energy is clamped so the additive compose into the HDR scene (done in
// composite.frag, BEFORE ACES) never white-outs the temporal history.
//
// godraysIntensity <= 0 (r_godrays 0) is handled in composite.frag (the result
// is simply never sampled), so this pass is only ever recorded when ON.

layout(set = 0, binding = 0) uniform sampler2D sceneTex;   // linear HDR scene color
layout(set = 0, binding = 1) uniform sampler2D depthTex;   // scene depth (NEAREST, data)

layout(push_constant) uniform Push {
    vec2  sunUV;        // sun position in screen UV [0..1] (projected on the CPU)
    float density;      // r_godrays_density: step length scale toward the sun (~0.5..1)
    float decay;        // r_godrays_decay:   per-step attenuation (~0.95..0.99)
    float weight;       // r_godrays_weight:  per-sample contribution weight
    float exposure;     // overall shaft exposure (folded intensity * tasteful scale)
    float threshold;    // bright-pass knee (linear luminance) for the occluder mask
    int   numSamples;   // march sample count (deterministic; cvar-tunable)
    int   sunOnScreen;  // 1 = sun projects in front of camera & near frame; 0 = skip
    float pad0, pad1, pad2;
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

// Toward-sun luminance, masked to FAR-distance (sky/bright-source) texels so
// foreground geometry occludes the shaft. depth ~1.0 (reverse-Z far) or the
// classic far plane both read as "distant"; we treat anything past a high
// threshold as the light/sky layer that may emit a shaft.
float occluder(vec2 uv) {
    vec3 c = texture(sceneTex, uv).rgb;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Soft bright-pass knee: only radiance above the threshold seeds a shaft.
    float bright = max(lum - pc.threshold, 0.0);
    // Depth gate: this codebase uses STANDARD Z (near=0, far=1; clear 1.0, LESS
    // compare). The sky / distant light sits at the FAR plane (depth near 1.0).
    // Fade the mask in as depth approaches the far plane so near geometry punches
    // holes (true occlusion -> shafts stab PAST the capital ship, not through it).
    float d = texture(depthTex, uv).r;
    float farMask = smoothstep(0.98, 1.0, d);  // 1 at far plane, 0 for near geometry
    return bright * farMask;
}

void main() {
    if (pc.sunOnScreen == 0 || pc.numSamples <= 0) { outColor = vec4(0.0); return; }

    // March from the current pixel toward the sun's screen position.
    vec2 delta = (pc.sunUV - vUV);
    // Step length scaled by density / sample count (classic formulation).
    delta *= (1.0 / float(pc.numSamples)) * pc.density;

    vec2 uv = vUV;
    float illum = 1.0;          // decays each step
    vec3  accum = vec3(0.0);

    // Fixed loop count (compile-time bound via the push int, deterministic).
    for (int i = 0; i < pc.numSamples; ++i) {
        uv += delta;
        // Clamp-to-edge sampling: outside [0,1] just reads the border, harmless.
        float m = occluder(clamp(uv, 0.0, 1.0));
        accum += vec3(m) * illum * pc.weight;
        illum *= pc.decay;
    }

    // Tint the shaft by the actual source color at the sun position so an orange
    // engine glow throws an orange shaft, the sun a warm-white one. Normalize the
    // source tint so brightness comes from `accum`, hue from the source.
    vec3 srcCol = texture(sceneTex, clamp(pc.sunUV, 0.0, 1.0)).rgb;
    float srcLum = max(dot(srcCol, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    vec3 tint = mix(vec3(1.0), srcCol / srcLum, 0.6);

    vec3 shaft = accum * tint * pc.exposure;
    // Energy clamp: never let the additive shaft blow past a sane HDR ceiling
    // (protects TAA history from white-out; the intro can still push it via cvars
    // but this caps a degenerate sun-fills-screen case).
    shaft = clamp(shaft, 0.0, 8.0);
    outColor = vec4(shaft, 1.0);
}
