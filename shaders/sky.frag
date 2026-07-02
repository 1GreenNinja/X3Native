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
// to mesh.frag's kSunColor. HDR PIPELINE: the sky now outputs LINEAR HDR radiance
// into the R16G16B16A16_SFLOAT scene target (same as mesh.frag); the shared ACES
// tonemap runs ONCE in composite.frag so sky + lit geometry stay on one response
// curve at the horizon seam. (The per-fragment tonemap here was moved out.)

layout(set = 0, binding = 0) uniform SkyUBO {
    mat4 invViewProj;   // unproject NDC -> world ray
    vec4 camPos;        // xyz = camera world position
    vec4 sunDir;        // xyz = normalized direction TOWARD the sun (matches lighting)
    vec4 sunColor;      // rgb = sun color (matches the directional light), a = intensity
    vec4 params;        // x = turbidity/haze, y = exposure, z = sky time (starfield), w = nebula strength
    vec4 zenith;        // rgb = overhead sky color (linear); per-scene
    vec4 horizon;       // rgb = horizon glow color (linear); per-scene
} sky;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

// ---- Procedural starfield. Additive + gated to dark skies, so it is INVISIBLE by
// day (Level1) and only blooms on a night sky. Rotates around Y by sky.params.z
// (seconds) for the slow "wheeling celestial sphere" once a scene advances time.
float starHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123); }
vec3 starField(vec3 dir, float t) {
    float c = cos(t * 0.02), s = sin(t * 0.02);              // slow celestial rotation
    vec3 d = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
    vec2 uv = vec2(atan(d.z, d.x) * 0.1591549 + 0.5,
                   asin(clamp(d.y, -1.0, 1.0)) * 0.3183099 + 0.5);   // lat-long
    vec3 acc = vec3(0.0);
    for (int o = 0; o < 2; ++o) {                            // two density layers
        float dens = (o == 0) ? 260.0 : 150.0;
        vec2 g = uv * dens;
        vec2 cell = floor(g);
        float h = starHash(cell + float(o) * 19.7);
        float present = step(0.955, h);                      // denser starfield (~4.5% of cells)
        vec2 f = fract(g) - 0.5;
        float pt = present * smoothstep(0.16, 0.0, length(f));
        float tw = 0.6 + 0.4 * sin(t * 2.0 + h * 31.4);      // twinkle
        float bright = (0.35 + 0.65 * starHash(cell + 7.1)) * tw;
        vec3 tint = mix(vec3(0.8, 0.85, 1.0), vec3(1.0, 0.9, 0.8), starHash(cell + 2.3));
        acc += pt * bright * tint * ((o == 0) ? 1.0 : 0.6);
    }
    return acc;
}

// ---- Procedural alien-night NEBULA (params.w > 0 only; default 0 = every
// existing sky byte-identical). Two ridged-FBM cloud fields — one TEAL, one
// ROSE — painted in the same lat-long space as the stars, additive over the
// night gradient and gated to DARK skies exactly like the starfield.
float nbHash(vec2 p) { return fract(sin(dot(p, vec2(41.3, 289.1))) * 43758.5453123); }
float nbNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    // Quintic (smootherstep) interpolant — no visible linear ramps at cell edges.
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(nbHash(i),                 nbHash(i + vec2(1, 0)), u.x),
               mix(nbHash(i + vec2(0, 1)),    nbHash(i + vec2(1, 1)), u.x), u.y);
}
float nbFbm(vec2 p) {
    // Rotate the domain ~37 deg every octave so the value-noise LATTICE never
    // lines up across octaves — kills the axis-aligned "rectangular patch"
    // artifact that a plain scale-by-2 fbm produces at low frequency.
    const mat2 R = mat2(0.80, 0.60, -0.60, 0.80);
    float a = 0.5, acc = 0.0;
    for (int i = 0; i < 6; ++i) { acc += a * nbNoise(p); p = R * p * 2.03 + 19.3; a *= 0.5; }
    return acc;
}
vec3 nebulaField(vec3 dir) {
    vec2 uv = vec2(atan(dir.z, dir.x) * 0.1591549 + 0.5,
                   asin(clamp(dir.y, -1.0, 1.0)) * 0.3183099 + 0.5);
    // Domain-warp the lookup so cloud EDGES billow (fluid, not lattice-shaped).
    vec2 w = vec2(nbFbm(uv * 3.0 + 5.0), nbFbm(uv * 3.0 + 61.0));
    vec2 quv = uv + 0.35 * (w - 0.5);
    // Two broad WARPED masks place a TEAL field and a ROSE field in different
    // regions of the sky; higher-freq detail fbm adds filamentary structure.
    // Wide smootherstep windows = soft feathered edges (patchy, not a wash, and
    // no hard rectangles).
    // HIGH thresholds -> the clouds occupy only the upper tail of the fbm, so most
    // of the sky stays DARK (stars read through) and the nebula comes in distinct
    // PATCHES — teal on one flank, rose on another (lab.jpg), not a full wash.
    float m1 = smoothstep(0.56, 0.92, nbFbm(quv * 2.2 + 11.0));
    float m2 = smoothstep(0.58, 0.94, nbFbm(quv * 1.8 + 47.0));
    float d1 = pow(nbFbm(quv * 6.0 +  3.0), 1.7);
    float d2 = pow(nbFbm(quv * 5.2 + 71.0), 1.7);
    vec3 teal = vec3(0.11, 0.44, 0.48) * (m1 * (0.30 + 1.4 * d1));   // brighter — no longer buried under rose
    vec3 rose = vec3(0.34, 0.10, 0.17) * (m2 * (0.28 + 1.3 * d2));
    return teal + rose;
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
    vec3 kZenith  = sky.zenith.rgb;    // overhead sky color (per-scene; default deep blue)
    vec3 kHorizon = sky.horizon.rgb;   // horizon glow color (per-scene; default pale blue)
    // Bias the blend toward the horizon (pow < 1 lifts the band) so the gradient
    // doesn't read as a flat 50/50 split.
    float grad = pow(t, 0.55);
    vec3 col = mix(kHorizon, kZenith, grad);

    // ---- Extra horizon haze: lift + desaturate the lowest strip of sky. ----
    float horizonBand = pow(1.0 - t, 8.0);
    vec3 hazeTint = clamp(kHorizon * 1.12, 0.0, 1.0);   // glow band follows the per-scene horizon color (cool + bright, not grey)
    col = mix(col, hazeTint, horizonBand * (0.35 + 0.45 * haze));

    // ---- Stars: additive, gated hard to DARK skies. Above the horizon always;
    // BELOW it only when there is no aerosol haze (haze == 0 reads as DEEP SPACE —
    // a space scene looking "down" sees stars, not a ground plane). Hazy ground
    // scenes are unchanged (the below-horizon weight is 0 once haze >= 0.5). ----
    {
        float skyLum = dot(col, vec3(0.299, 0.587, 0.114));
        float night  = pow(clamp(1.0 - skyLum, 0.0, 1.0), 4.0);   // only very dark skies
        float aboveW = smoothstep(0.0, 0.12, up);
        float spaceW = clamp(1.0 - haze * 2.0, 0.0, 1.0);
        // Alien nebula BEHIND the stars (params.w gates it; 0 == off/no-op).
        float nebulaAmt = clamp(sky.params.w, 0.0, 2.0);
        if (nebulaAmt > 0.0)
            col += nebulaField(dir) * (nebulaAmt * night) * max(aboveW, spaceW);
        col += starField(dir, sky.params.z) * (1.5 * night) * max(aboveW, spaceW);
    }

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
    // The earth tone follows the AEROSOL HAZE: hazy daylight keeps the original
    // muted ground (byte-for-byte at haze >= 0.5); haze 0 == DEEP SPACE, where the
    // "ground" hemisphere falls to the per-scene horizon floor (near-black) so
    // space scenes don't render a grey dirt plane under the stars.
    if (up < 0.0) {
        float groundW = clamp(haze * 2.0, 0.0, 1.0);   // 0 in deep space -> NO ground blend
        vec3 ground = vec3(0.16, 0.15, 0.13);
        float belowT = clamp(-up * 4.0, 0.0, 1.0);
        col = mix(col, ground, belowT * groundW);      // haze>=0.5: original ground exactly
    }

    // Exposure applied here; LINEAR HDR output (tonemap moved to composite.frag,
    // applied once after bloom — matching the lit-geometry path's response).
    col *= exposure;
    outColor = vec4(col, 1.0);
}
