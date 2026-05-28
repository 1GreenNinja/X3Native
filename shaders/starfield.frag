#version 450

// Procedural starfield — fragment stage. Hashes the world-space view direction
// for the current pixel into a stable star pattern that's world-space anchored
// (so the stars stay PARALLAX-correct as the camera rotates / translates) and
// twinkles via a per-star hashed phase.
//
// Approach: divide the unit sphere into "voxel" cells of a 3D integer lattice
// (in direction space). Each cell is hashed into a single 32-bit value. A high
// threshold gates whether the cell contains a star (most cells -> empty space).
// The hash itself is decoded into a sub-cell position so stars don't snap to
// the lattice grid. A 2D distance in the dominant-axis plane of the direction
// gives an antialiased disk. A sine of (time * twinkleSpeed + hashedPhase)
// modulates each star's brightness so they twinkle independently.
//
// Tuning fields (matched to the SkyStars::Tuning struct on the C++ side):
//   density    higher = finer lattice -> more stars per steradian
//   radius     pixel-relative star half-size
//   threshold  hash threshold (0..1) -- closer to 1 = sparser
//   twinkleHz  twinkle frequency (Hz-ish)
//   baseColor  global star tint (slight blue-white)

layout(set = 0, binding = 0) uniform StarfieldUBO {
    mat4 invViewProj;   // unproject centered-NDC -> world ray
    vec4 camPos;        // xyz = camera world position (w = unused)
    vec4 params0;       // x = density, y = radius, z = threshold, w = twinkleHz
    vec4 params1;       // xyz = baseColor (RGB), w = timeSec
} sf;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

// Cheap 3D -> [0,1) hash. Sufficiently uniform for a starfield's purposes
// (visual stability across the cell, no obvious axis-aligned banding).
float hash3(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

void main() {
    // Reconstruct world-space view ray (same scheme as sky.frag). Centered-NDC
    // in vNdc is already in [-1,1].
    vec4 nearH = sf.invViewProj * vec4(vNdc, 0.0, 1.0);
    vec4 farH  = sf.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 nearW = nearH.xyz / nearH.w;
    vec3 farW  = farH.xyz  / farH.w;
    vec3 dir   = normalize(farW - nearW);   // world-space ray from the eye

    float density   = max(sf.params0.x, 1.0);
    float radiusPx  = max(sf.params0.y, 0.05);
    float threshold = clamp(sf.params0.z, 0.0, 1.0);
    float twinkleHz = max(sf.params0.w, 0.0);
    vec3  baseRGB   = sf.params1.xyz;
    float timeSec   = sf.params1.w;

    // Voxel-grid hashing in direction space. The density scales the lattice; a
    // single star is dropped per "occupied" cell, with sub-cell jitter pulled
    // from the same hash.
    vec3 scaledDir = dir * density;
    vec3 voxel = floor(scaledDir);

    // Layer 1 -- bright stars (sparse). Triple the threshold gap so the cell
    // distribution naturally yields ~1% occupied.
    float h = hash3(voxel + vec3(0.1));
    vec3 col = vec3(0.0);
    if (h > threshold) {
        // Sub-cell offset: two hashed reals in [0,1) shifted to [-0.5, 0.5].
        float ju = fract(h * 17.123) - 0.5;
        float jv = fract(h * 31.731) - 0.5;
        vec3 cellFrac = fract(scaledDir) - 0.5;
        // Distance to the (jittered) star center within the cell (planar
        // approximation — the dominant axis is implicit in floor/fract on the
        // 3D lattice, which keeps stars from skewing near the poles).
        float du = cellFrac.x - ju;
        float dv = cellFrac.y - jv;
        float dw = cellFrac.z - (fract(h * 11.0) - 0.5);
        float d  = sqrt(du*du + dv*dv + dw*dw);
        // Antialiased star disk.
        float brightness = smoothstep(radiusPx, 0.0, d);
        // Per-star twinkle phase from the same hash.
        float phase = h * 6.2831853;
        float tw    = 0.55 + 0.45 * sin(timeSec * twinkleHz * 6.2831853 + phase);
        brightness *= tw;
        // Per-star color jitter (slight warm/cool variance around baseColor).
        float warmCool = fract(h * 53.0);
        vec3  tint = mix(vec3(0.85, 0.9, 1.05), vec3(1.05, 0.95, 0.85), warmCool);
        col = baseRGB * tint * brightness;
    }

    // Layer 2 -- a denser, dimmer dust layer at 2x density (the "Milky Way"
    // ambient sparkle). Same hash, different salt, higher threshold so it's
    // sparse too, but each star is fainter.
    {
        vec3 scaledDir2 = dir * density * 2.0;
        vec3 voxel2 = floor(scaledDir2);
        float h2 = hash3(voxel2 + vec3(7.7));
        float th2 = mix(threshold, 1.0, 0.6);   // even sparser layer
        if (h2 > th2) {
            float ju = fract(h2 * 17.0) - 0.5;
            float jv = fract(h2 * 29.0) - 0.5;
            float jw = fract(h2 * 41.0) - 0.5;
            vec3 cf  = fract(scaledDir2) - 0.5;
            float du = cf.x - ju, dv = cf.y - jv, dw = cf.z - jw;
            float d  = sqrt(du*du + dv*dv + dw*dw);
            float brightness = smoothstep(radiusPx * 0.6, 0.0, d) * 0.35;
            float phase = h2 * 6.2831853 + 1.7;
            float tw    = 0.6 + 0.4 * sin(timeSec * twinkleHz * 6.2831853 * 0.7 + phase);
            col += baseRGB * brightness * tw;
        }
    }

    outColor = vec4(col, 1.0);
}
