#version 450

// Analytic-sky fragment shader (open-world track, task A).
//
// A physically-PLAUSIBLE (not physically-exact) daytime sky, evaluated per pixel
// from the reconstructed world-space view ray:
//   * A zenith->horizon gradient (deep blue overhead easing to a pale, slightly
//     warm horizon haze) — a cheap stand-in for Rayleigh out-scattering darkening
//     with altitude.
//   * A horizon haze band that lifts + desaturates the lowest part of the sky
//     (aerosol/Mie-ish forward glow near the ground).
//   * A sun DISK (sharp, bright core) plus a wide forward GLOW (Mie-like halo)
//     placed at the engine's directional sun direction so the lit world and the
//     backdrop agree.
//   * A faint ground/below-horizon tint so looking down doesn't show raw black.
//
// The sun direction + color come from the sky UBO; main.cpp feeds the SAME
// normalize(0.4,1,0.3) the shadow pass + mesh.frag use, and a sun color matched
// to mesh.frag's kSunColor. The result is ACES-tonemapped exactly like mesh.frag
// so sky and lit geometry sit in the same response curve at the horizon seam.

layout(set = 0, binding = 0) uniform SkyUBO {
    mat4 invViewProj;   // unproject NDC -> world ray
    vec4 camPos;        // xyz = camera world position
    vec4 sunDir;        // xyz = normalized direction TOWARD the sun (matches lighting)
    vec4 sunColor;      // rgb = sun color (matches the directional light), a = intensity
    vec4 params;        // x = turbidity/haze amount, y = exposure, z/w = reserved
} sky;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

// Same ACES filmic approximation (Narkowicz) used by mesh.frag, so the sky and
// the lit geometry share one tonemapping response (no seam at the horizon).
vec3 tonemapACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    // Reconstruct the world-space view ray for this pixel. Unproject a near and a
    // far NDC point through the inverse viewProj, then take their difference.
    vec4 nearH = sky.invViewProj * vec4(vNdc, 0.0, 1.0);
    vec4 farH  = sky.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 nearW = nearH.xyz / nearH.w;
    vec3 farW  = farH.xyz  / farH.w;
    vec3 dir   = normalize(farW - nearW);   // world-space ray from the eye

    vec3 sunDir = normalize(sky.sunDir.xyz);
    float haze  = clamp(sky.params.x, 0.0, 1.0);
    float exposure = max(sky.params.y, 0.0001);

    // Elevation: 1 at zenith (dir.y = 1), 0 at horizon, negative below.
    float up = dir.y;
    float t  = clamp(up, 0.0, 1.0);

    // ---- Sky gradient (zenith -> horizon). Linear-ish colors; ACES at the end. ----
    // Deep blue overhead; pale, faintly warm haze at the horizon.
    const vec3 kZenith  = vec3(0.10, 0.28, 0.66);   // overhead blue
    const vec3 kHorizon = vec3(0.62, 0.74, 0.92);   // pale horizon haze
    // Bias the blend toward the horizon (pow < 1 lifts the band) so the gradient
    // doesn't read as a flat 50/50 split.
    float grad = pow(t, 0.55);
    vec3 col = mix(kHorizon, kZenith, grad);

    // ---- Extra horizon haze: lift + desaturate the lowest strip of sky. ----
    float horizonBand = pow(1.0 - t, 8.0);
    vec3 hazeTint = vec3(0.78, 0.82, 0.88);
    col = mix(col, hazeTint, horizonBand * (0.35 + 0.45 * haze));

    // ---- Sun disk + glow, placed at the directional sun. ----
    float cosAngle = clamp(dot(dir, sunDir), -1.0, 1.0);
    vec3 sunRGB = sky.sunColor.rgb;
    float sunI  = max(sky.sunColor.a, 0.0);
    // Wide Mie-like forward glow: a high-power cosine lobe around the sun. Grows
    // toward the horizon (more aerosol path) for a believable late-day flare.
    float glow = pow(max(cosAngle, 0.0), 8.0)  * 0.20
               + pow(max(cosAngle, 0.0), 256.0) * 0.55;
    col += sunRGB * glow * sunI;
    // Sharp disk: a small angular cap (~0.5 deg) with a soft antialiased edge.
    // cos(0.5deg) ~ 0.99996; widen slightly so it reads on a 720p capture.
    float diskInner = 0.99986;   // ~0.95 deg
    float diskOuter = 0.99965;   // soft outer falloff
    float disk = smoothstep(diskOuter, diskInner, cosAngle);
    col += sunRGB * disk * (4.0 * sunI);

    // ---- Below-horizon: ease into a muted ground tint so down-views aren't black.
    if (up < 0.0) {
        vec3 ground = vec3(0.16, 0.15, 0.13);
        float belowT = clamp(-up * 4.0, 0.0, 1.0);
        col = mix(col, ground, belowT);
    }

    // Exposure + shared ACES response, matching the lit-geometry path.
    col = tonemapACES(col * exposure);
    outColor = vec4(col, 1.0);
}
