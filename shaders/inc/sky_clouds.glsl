// ===========================================================================
// SHARED CLOUD DENSITY — one function for the deck you SEE and the shade it
// CASTS. Included by BOTH shaders/sky.frag (the sky layer) and shaders/
// mesh.frag (cloud shadows on the ground, task #27). PAIRED VALUES ARE ONE
// VALUE: the scale, altitude, drift and coverage remap live HERE and only
// here, so the ground can never darken under a sky that looks different.
//
// THE SHARD RECEIPT (2026-08-16, "this is a cloud now / It looks AWWFUL"):
// the previous cloud layer hashed with fract(sin(dot(p, k)) * 43758.5). On
// the GPU, sin() is a fast approximation whose output has NO fractional
// precision once |x| grows past fp32's accurate range — and the fbm octave
// chain pushes dot(p, k) into the tens of thousands a few kilometres from
// the origin. fract() of that garbage is CONSTANT-ish per noise cell, so
// every cell rendered as a flat, hard-edged RECTANGLE: the "angular shard"
// sky. A CPU repro of the identical fbm with an accurate sin (shots_clouds/
// repro_cloud.py) renders soft blobs — the math was fine, the hash was not.
// The fix is the same one the starfield in sky.frag already learned: a
// sin-FREE hash (Hoskins-style hash12), which is magnitude-safe.
// ===========================================================================

// Octave count is compile-time per consumer: the sky pays for 5, the ground
// shadow reads the same shapes at 3 (the projected shade is soft-edged, the
// last two octaves are sub-shadow detail). Define before including to override.
#ifndef CLOUD_FBM_OCTAVES
#define CLOUD_FBM_OCTAVES 5
#endif

// The layer: a flat cumulus deck at altitude. sky.frag intersects the view
// ray with this plane; mesh.frag projects the fragment onto it along the sun.
const float kCloudPlaneAlt   = 1400.0;    // m (~4,600 ft cumulus base)
const float kCloudNoiseScale = 0.00055;   // world metres -> noise units
// Drift: noise-units/second, deliberately off-axis. THE WIND. The same vector
// moves the sky deck and its ground shade (both sample at t * this).
const vec2  kCloudDrift      = vec2(0.011, 0.004);

// Sin-free 2D->1D hash (Hoskins hash12). fract() keeps every intermediate in
// fp32's sweet spot regardless of input magnitude — this is the shard fix.
float cloudHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Value noise over the integer lattice, smoothstep-interpolated.
float cloudNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(cloudHash(i + vec2(0, 0)), cloudHash(i + vec2(1, 0)), u.x),
               mix(cloudHash(i + vec2(0, 1)), cloudHash(i + vec2(1, 1)), u.x), u.y);
}

// fBm with a ROTATION between octaves (kills the axis-aligned lattice look
// value noise otherwise carries) and a FRACTIONAL octave count: `octaves`
// fades the high-frequency terms out smoothly, so a distant sample can drop
// detail that would alias into horizon static without popping a transition.
//
// NORMALIZED by the live weight sum — this is load-bearing, not cosmetic.
// Unnormalized, a 3-octave sum tops out at 0.875 where 5 octaves reach 0.969,
// so every consumer running fewer octaves read LESS COVERAGE through the same
// threshold: the ground shadow (3 octaves) saw 97% full sun under a sky that
// drew 47% cloud, and the sky's own distance fade (oct -> 2) thinned the deck
// toward the horizon instead of crowding it. Measured in
// shots_clouds/verify_new_field.py — one field, one amplitude, any octave count.
float cloudFbm(vec2 p, float octaves) {
    const mat2 kRot = mat2(0.80, 0.60, -0.60, 0.80) * 2.03;  // rotate + lacunarity
    float a = 0.5, sum = 0.0, norm = 0.0;
    for (int k = 0; k < CLOUD_FBM_OCTAVES; ++k) {
        float w = clamp(octaves - float(k), 0.0, 1.0);
        sum  += a * w * cloudNoise(p);
        norm += a * w;
        p = kRot * p + 11.7;
        a *= 0.5;
    }
    return sum / max(norm, 1e-4);
}

// Cloud density in [0,1] at a world XZ on the deck plane.
//   cover   0..1 — SkyParams::cloud. 0 = clear, 1 = solid overcast.
//   t       seconds — SkyParams time (setSkyTime); drives the drift.
//   octaves detail level, up to CLOUD_FBM_OCTAVES (fractional ok).
// Shape: domain-warped fbm (the warp is what billows the blobs into weather)
// remapped through a coverage threshold — low cover leaves real blue gaps
// between distinct cumulus, high cover merges them into a deck.
float cloudCoverAt(vec2 worldXZ, float cover, float t, float octaves) {
    vec2 p = worldXZ * kCloudNoiseScale + kCloudDrift * t;
    vec2 w = vec2(cloudFbm(p * 0.5 + 17.3, octaves),
                  cloudFbm(p * 0.5 - 9.1,  octaves));
    float d  = cloudFbm(p + (w - 0.5) * 1.6, octaves);
    // Threshold CALIBRATED against the normalized field, numerically (see
    // shots_clouds/verify_new_field.py): solved lo per cover for the target
    // sky-fraction ladder (0.42 -> ~47% scattered cumulus, 0.66 -> ~80%
    // broken overcast, 0.94 -> ~97% storm deck), then fit. Quadratic because
    // the required curve steepens at high cover — a linear mix leaves the
    // storm deck full of holes.
    float c  = clamp(cover, 0.0, 1.0);
    float lo = 0.66 - 0.20 * c - 0.29 * c * c;
    return smoothstep(lo, lo + 0.30, d);
}

// Ground-shade factor for a lit fragment (task #27): project the fragment
// onto the deck along the TOWARD-SUN direction, read the same density the
// sky draws there, and dim the DIRECT sun by its optical depth (ambient is
// untouched — cloud shade outdoors is skylight, not darkness).
// Multiply the sun radiance by the result. strength 0 (or cover 0) == 1.0.
float cloudShadowFactor(vec3 worldPos, vec3 sunDir, float cover, float t, float strength) {
    if (cover <= 0.001 || strength <= 0.0 || sunDir.y <= 0.05) return 1.0;
    vec2 wp = worldPos.xz + sunDir.xz * ((kCloudPlaneAlt - worldPos.y) / max(sunDir.y, 0.2));
    float d = cloudCoverAt(wp, cover, t, float(CLOUD_FBM_OCTAVES));
    // Same optical curve the sky's alpha uses (sky.frag): thin edge = light
    // dapple, core = deep shade. Keep the two curves TOGETHER.
    float opt = 1.0 - exp(-d * 5.0);
    return 1.0 - strength * opt;
}
