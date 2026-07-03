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
    vec4 params;        // x = turbidity/haze amount, y = exposure, z/w = reserved
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

// ---- Value-noise fBm for the nebula band (alien-night sky, §2 teal+rose). Low
// frequency, cheap; sampled in lat-long UV so it wraps with the star sphere.
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = starHash(i + vec2(0.0, 0.0));
    float b = starHash(i + vec2(1.0, 0.0));
    float c = starHash(i + vec2(0.0, 1.0));
    float d = starHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float nebulaFbm(vec2 uv) {
    float s = 0.0, amp = 0.5, frq = 1.0;
    for (int i = 0; i < 4; ++i) { s += amp * vnoise(uv * frq); frq *= 2.03; amp *= 0.5; }
    return s;
}

// ---- One crescent moon disk at world-direction md, angular radius `rad` (in
// dot-space: larger = smaller moon), lit from `phase`. Adds a soft halo + a hard
// body with a shaded terminator so it reads as a sphere, not a sticker.
vec3 moon(vec3 dir, vec3 md, float rad, vec3 tint, vec3 phase) {
    float cd = dot(dir, md);
    // Soft outer halo (tight + faint) — the moon sits in a little glow, not a blob.
    float halo = pow(max(cd, 0.0), 5000.0) * 0.22;
    // Body: a crisp-edged disk.
    float body = smoothstep(rad - 0.00012, rad + 0.00004, cd);
    // Reconstruct the near-hemisphere surface normal so the phase light carves a real
    // TERMINATOR across the disc (reads as a lit sphere, not a flat sticker — §8).
    float angMax = acos(clamp(rad, -1.0, 1.0));
    vec3 tangent = normalize(dir - md * cd + vec3(1e-6));
    float fromCenter = clamp(acos(clamp(cd, -1.0, 1.0)) / max(angMax, 1e-5), 0.0, 1.0);
    vec3 nrm = normalize(md * sqrt(max(1.0 - fromCenter * fromCenter, 0.0)) + tangent * fromCenter);
    float lit = dot(nrm, phase);
    // Hard-ish crescent: dark side stays deep (earthshine floor), lit side rolls up.
    float shade = 0.04 + 0.96 * smoothstep(-0.15, 0.55, lit);
    return tint * (body * shade * 1.35 + halo);
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
        col += starField(dir, sky.params.z) * (1.5 * night) * max(aboveW, spaceW);

        // ---- ALIEN-NIGHT layer (§2): teal+rose nebula band + two crescent moons.
        // Gated by params.w (nebula strength) AND to dark skies, so day/dusk scenes
        // with w==0 are byte-for-byte unchanged. Rotates with the star sphere.
        float neb = clamp(sky.params.w, 0.0, 1.0);
        if (neb > 0.001) {
            float c2 = cos(sky.params.z * 0.02), s2 = sin(sky.params.z * 0.02);
            vec3 rd = vec3(c2 * dir.x + s2 * dir.z, dir.y, -s2 * dir.x + c2 * dir.z);
            vec2 nuv = vec2(atan(rd.z, rd.x) * 0.1591549 + 0.5,
                            asin(clamp(rd.y, -1.0, 1.0)) * 0.3183099 + 0.5);
            // A soft diagonal galactic band + fBm clouds.
            float band = exp(-pow((nuv.y - 0.52 - 0.10 * sin(nuv.x * 6.2831)) * 6.0, 2.0));
            float clouds = nebulaFbm(nuv * vec2(6.0, 3.0) + 3.1);
            clouds = smoothstep(0.45, 0.95, clouds);
            float amt = (band * 0.6 + clouds * 0.5) * neb * night * aboveW;
            // Teal in the troughs -> rose in the peaks (the crystal-spectrum night).
            vec3 teal = vec3(0.045, 0.19, 0.27);
            vec3 rose = vec3(0.30, 0.09, 0.23);
            vec3 nebCol = mix(teal, rose, clamp(clouds * 1.15, 0.0, 1.0));
            col += nebCol * amt * 1.05;

            // Two crescent moons, lit from a shared GRAZING phase direction so the
            // terminator crosses each disc (a real crescent). Fixed world directions.
            float mgate = neb * night;
            vec3 phase = normalize(vec3(-0.86, 0.10, 0.30));
            vec3 mA = normalize(vec3( 0.42, 0.34, -0.84));   // larger, higher
            vec3 mB = normalize(vec3(-0.66, 0.20,  0.62));   // smaller, opposite
            col += moon(dir, mA, 0.99965, vec3(0.85, 0.86, 0.92), phase) * mgate;
            col += moon(dir, mB, 0.99985, vec3(0.80, 0.72, 0.74), phase) * mgate;
        }
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
