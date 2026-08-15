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

// ===========================================================================
// CLOUDS — a lit layer at altitude, not a texture on a dome.
//
// THE ONE DECISION THAT MATTERS: the view ray is intersected with a horizontal
// PLANE at kCloudAlt and the noise is sampled at that world position. That is
// what makes clouds behave: overhead they are broad and slow, toward the
// horizon they compress and crowd together because the ray grazes the plane at
// a shallow angle. Sampling noise by DIRECTION instead — the obvious shortcut —
// gives a bowl of cotton wool that slides with the camera and never reads as
// distance. Perspective is the whole effect.
//
// Everything else serves that:
//   * fBm with a DOMAIN WARP so the shapes are billowed rather than griddy.
//     Plain fBm reads as static; warping the sample point by another fBm is
//     what turns blobs into something with flow in it.
//   * A COVERAGE THRESHOLD, not a fog. Real fair-weather sky is mostly blue
//     with distinct clouds in it, so the noise is remapped through a smoothstep
//     whose floor rises with the cover parameter. Low cover = separate cumulus
//     with blue between them; high cover = they merge into overcast.
//   * SUN-SIDE SILVERING. A cloud is lit from one side: the density GRADIENT
//     along the sun direction drives a bright rim, so the edge facing the sun
//     glows and the far side stays grey-blue. Without this clouds are flat
//     stickers, and no amount of noise detail fixes that.
//   * HORIZON FADE. The layer dissolves into the haze band near the horizon so
//     there is no hard line where the plane runs out.
// ===========================================================================

float cloudHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float cloudNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);          // smoothstep interpolant
    return mix(mix(cloudHash(i + vec2(0,0)), cloudHash(i + vec2(1,0)), u.x),
               mix(cloudHash(i + vec2(0,1)), cloudHash(i + vec2(1,1)), u.x), u.y);
}

float cloudFbm(vec2 p) {
    float a = 0.5, sum = 0.0;
    for (int k = 0; k < 5; ++k) { sum += a * cloudNoise(p); p *= 2.03; a *= 0.5; }
    return sum;
}

// Density in [0,1] at a world XZ on the cloud plane.
float cloudDensity(vec2 wp, float cover, float t) {
    vec2 drift = vec2(t * 0.006, t * 0.0022);          // slow, and not axis-aligned
    vec2 p = wp * 0.00055 + drift;
    // Domain warp: displace the sample by a coarser fBm. This is the difference
    // between "noise" and "weather".
    vec2 w = vec2(cloudFbm(p * 0.5 + 17.3), cloudFbm(p * 0.5 - 9.1));
    float d = cloudFbm(p + (w - 0.5) * 1.6);
    // Coverage: raise the floor with `cover` so low cover leaves real blue gaps.
    float lo = mix(0.62, 0.24, clamp(cover, 0.0, 1.0));
    return smoothstep(lo, lo + 0.22, d);
}


// ---- Procedural starfield. Additive + gated to dark skies, so it is INVISIBLE by
// day (Level1) and only blooms on a night sky. Rotates around Y by sky.params.z
// (seconds) for the slow "wheeling celestial sphere" once a scene advances time.
//
// REBUILT (owner: "the stars need to be tinier points.. and varied"). What was
// wrong with the old field, and what replaced it:
//
//  * LAT-LONG CELLS -> CUBE-FACE CELLS. The old grid lived in equirectangular UV,
//    where a cell's angular width collapses as cos(latitude): cells stretched into
//    smeared DASHES toward the poles and pinched into a visible seam overhead.
//    Stars are now diced on the CUBE FACE the ray points at, so every cell covers
//    the same solid angle in every direction — round points, no pole smear, no seam.
//  * SIZE. The old star was a disc of radius 0.16 CELL at density 260 across the
//    full 360deg sweep — a ~1.4deg cell, so the "star" was ~6 px of solid white at
//    65deg FOV. That is the snow/blob look. Density is now ~5x finer and the radius
//    is a POWER-LAW: almost every star is a sub-pixel point, a handful are 1-2 px.
//  * BRIGHTNESS. Was near-uniform (0.35..1.0). Now pow(h, 3.5) — a real magnitude
//    distribution: the sky is dominated by faint stars and a few standouts carry
//    the eye.
//  * COLOR. Was a 2-stop blue<->warm lerp. Now a stellar TEMPERATURE ramp
//    (blue-white -> white -> yellow-white -> amber -> faint red), weighted toward
//    white, and desaturated toward the faint end (dim stars read achromatic to the
//    eye, and saturated dim pixels look like dead subpixels).
//  * DISTRIBUTION. Uniform scatter reads synthetic. Presence probability is now
//    modulated by a GALACTIC BAND plus a coarse clumping hash, so the field
//    clusters and drifts instead of dusting evenly.
//  * NO SHIMMER. Every property is a deterministic hash of the CELL (never of time
//    or screen position), and the point uses a smooth gaussian-ish falloff rather
//    than a hard disc, so shrinking to sub-pixel FADES instead of aliasing into
//    sparkle. The twinkle is slow and shallow (0.88..1.0) — it must never read as
//    static or fireflies.
// A decorrelated 2D hash. The old sin(dot(p,k)) hash correlates badly along axes —
// it laid the stars into faint ROWS, which is exactly the lattice look we are trying
// to kill. This one mixes both components through each other first.
float starHash(vec2 p) {
    vec2 q = fract(p * vec2(443.8975, 397.2973));
    q += dot(q, q.yx + 19.19);
    return fract((q.x + q.y) * q.x);
}

// Direction -> (cube face index, in-face UV in [0,1]). Equal-solid-angle-ish cells
// with no pole pinch (the whole point of moving off the lat-long grid).
vec2 starCubeUV(vec3 d, out float face) {
    vec3 a = abs(d);
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z)      { face = d.x > 0.0 ? 0.0 : 1.0; uv = vec2(-d.z, d.y) / a.x; }
    else if (a.y >= a.z)               { face = d.y > 0.0 ? 2.0 : 3.0; uv = vec2( d.x, d.z) / a.y; }
    else                               { face = d.z > 0.0 ? 4.0 : 5.0; uv = vec2( d.x, d.y) / a.z; }
    return uv * 0.5 + 0.5;
}

// Stellar temperature ramp, weighted toward white. u in [0,1].
vec3 starTint(float u) {
    vec3 blueWhite = vec3(0.72, 0.80, 1.00);
    vec3 white     = vec3(0.96, 0.97, 1.00);
    vec3 yellowW   = vec3(1.00, 0.96, 0.86);
    vec3 amber     = vec3(1.00, 0.82, 0.62);
    vec3 red       = vec3(1.00, 0.68, 0.55);
    // Most of the ramp sits in the white / yellow-white middle; the extremes are rare.
    if (u < 0.18) return mix(blueWhite, white,   u / 0.18);
    if (u < 0.62) return mix(white,     yellowW, (u - 0.18) / 0.44);
    if (u < 0.88) return mix(yellowW,   amber,   (u - 0.62) / 0.26);
    return              mix(amber,      red,     (u - 0.88) / 0.12);
}

vec3 starField(vec3 dir, float t) {
    float c = cos(t * 0.02), s = sin(t * 0.02);              // slow celestial rotation
    vec3 d = normalize(vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z));

    // GALACTIC BAND: a great circle the field clumps along, so the sky has structure
    // instead of an even dusting. Tilted off the horizon so it reads as a real plane.
    vec3  bandN  = normalize(vec3(0.36, 0.86, -0.36));       // pole of the band
    float lat    = abs(dot(d, bandN));                        // 0 = on the band, 1 = at its pole
    float band   = exp(-lat * lat * 7.0);                     // dense on the plane, sparse off it

    vec3 acc = vec3(0.0);
    // Two layers: a fine field of faint points + a sparser layer of the few brighter
    // stars that actually carry the composition.
    for (int o = 0; o < 2; ++o) {
        // Cell size is chosen in SCREEN terms: a cube face spans 90deg, so a density
        // of 300 gives a ~0.3deg cell = ~6 px at 65deg FOV / 1280 px. The star radius
        // below is a fraction of THAT, which lands the faint majority at ~0.6 px and
        // the rare bright ones at ~1.8 px — tiny points, but points that actually
        // resolve instead of aliasing into sparkle between samples.
        float dens = (o == 0) ? 300.0 : 120.0;               // cells per cube face
        float face;
        vec2 uv = starCubeUV(d, face);
        vec2 g  = uv * dens;
        // Offset each face/layer into its own hash region — DIFFERENT offsets per axis,
        // or the two components stay correlated and the field bands up again.
        vec2 cell = floor(g) + vec2(face * 131.0 + float(o) * 47.0,
                                    face *  57.3 + float(o) * 23.7);

        float h = starHash(cell);
        // Coarse clumping: a low-frequency hash that locally raises/lowers density.
        float clump = starHash(floor(g / 9.0) + vec2(face * 17.0 + float(o) * 5.0,
                                                     face * 29.5 + float(o) * 9.3));
        // Presence: base rarity, boosted on the galactic band and in clumps.
        float thresh = (o == 0) ? 0.930 : 0.9880;
        thresh -= band * 0.030 + clump * 0.012;
        float present = step(thresh, h);
        if (present < 0.5) continue;

        // Per-star deterministic properties (hash of the CELL — never of time or of
        // gl_FragCoord — so nothing crawls or shimmers as the camera moves).
        float hs = starHash(cell + 7.1);    // size
        float hb = starHash(cell + 2.3);    // brightness
        float hc2 = starHash(cell + 5.7);   // color temperature
        float hj1 = starHash(cell + 11.3);  // jitter x
        float hj2 = starHash(cell + 19.9);  // jitter y

        // JITTER the star off the cell centre, or the field reads as a lattice.
        vec2 centre = vec2(hj1, hj2) * 0.7 + 0.15;
        vec2 f = fract(g) - centre;

        // SIZE: power-law. pow(h, 5) keeps the overwhelming majority at the floor
        // (sub-pixel) and lets a rare few grow. Layer 1 is the "bright star" layer.
        float rad = mix(0.100, (o == 0) ? 0.230 : 0.330, pow(hs, 5.0));
        // BRIGHTNESS: pow(h, 3.5) -> a magnitude-like distribution, mostly faint.
        float mag = pow(hb, 3.5);
        float bright = mix(0.15, 1.0, mag) * ((o == 0) ? 0.90 : 1.7);

        // SOFT point: a gaussian-ish falloff, NOT a hard disc. This is what lets a
        // sub-pixel star fade out gracefully instead of aliasing into a sparkle.
        float r2 = dot(f, f) / max(rad * rad, 1e-8);
        float pt = exp(-r2 * 2.0);
        // A whisper of a core so the brightest few read as points, not fuzz.
        pt += 0.30 * exp(-r2 * 8.0);

        // TWINKLE: slow + shallow. Never static, never fireflies.
        float tw = 0.88 + 0.12 * sin(t * 0.35 + h * 41.7);

        // Dim stars desaturate toward white (the eye sees faint stars achromatic).
        vec3 tint = mix(vec3(0.94, 0.96, 1.0), starTint(hc2), 0.35 + 0.65 * mag);

        acc += pt * bright * tw * tint;
    }

    // A faint MILKY glow along the band — the unresolved stars. Very dim; it gives
    // the field depth without ever reading as fog.
    acc += vec3(0.55, 0.60, 0.78) * band * 0.012;
    return acc;
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
    }

    vec3 sunRGB = sky.sunColor.rgb;

    // ---- CLOUDS: composited over the sky, under the sun disk. ---------------
    // Placed BEFORE the sun disk so the disk still burns through in front of a
    // thin edge, and AFTER the stars so a night sky keeps its clouds dark.
    {
        float cover = clamp(sky.params.w, 0.0, 1.0);
        // haze < 0.5 shades toward deep space; no clouds out there.
        float atmo  = clamp(haze * 2.0, 0.0, 1.0);
        if (cover > 0.001 && atmo > 0.001 && up > 0.0) {
            const float kCloudAlt = 1400.0;                 // ~4,600 ft
            float distToPlane = kCloudAlt / max(up, 0.02);   // ray -> plane
            vec2  wp = sky.camPos.xz + dir.xz * distToPlane;
            float t  = sky.params.z;

            float d = cloudDensity(wp, cover, t);

            // SUN-SIDE SILVERING from the density gradient along the sun's XZ
            // heading: the edge facing the sun lights up, the far side does not.
            vec2  sunXZ = normalize(sunDir.xz + vec2(1e-4));
            float dAhead = cloudDensity(wp + sunXZ * 900.0, cover, t);
            float rim    = clamp((d - dAhead) * 2.2, 0.0, 1.0);

            // Body colour: cool grey-white, warmed toward the sun's own colour.
            vec3 lit   = mix(vec3(0.62, 0.66, 0.74), sky.sunColor.rgb * 1.15, 0.35);
            vec3 shade = vec3(0.30, 0.33, 0.40);
            float sunAlign = pow(max(dot(dir, sunDir), 0.0), 6.0);
            vec3 cloudCol = mix(shade, lit, 0.35 + 0.65 * rim) + sunRGB * sunAlign * 0.55 * rim;

            // Fade into the horizon haze, and ease in from the zenith so the
            // layer has no hard edge where the plane runs out.
            float horizonFade = smoothstep(0.02, 0.22, up);
            float alpha = d * horizonFade * atmo;
            col = mix(col, cloudCol, clamp(alpha, 0.0, 1.0));
        }
    }

    // ---- Sun disk + glow, placed at the directional sun. ----
    float cosAngle = clamp(dot(dir, sunDir), -1.0, 1.0);
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
