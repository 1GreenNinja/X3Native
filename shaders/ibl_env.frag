#version 450

// IBL environment capture: render the ANALYTIC SKY (the same model as
// shaders/sky.frag) into one face of a cubemap. Driven per-face by a push
// constant that carries the cube-face direction basis (a forward + the two
// screen-axis vectors), so the fullscreen-triangle UV maps to a world ray. The
// sky parameters come from a small UBO that mirrors SkyParams. The output is
// LINEAR HDR radiance (R16G16B16A16_SFLOAT) — the scene's environment for IBL.
//
// CLEAN-ROOM: the sky body below is a copy of the public analytic-sky model in
// sky.frag (zenith/horizon gradient + Mie sun glow/disk + starfield + ground
// tint). Kept in sync by hand; if sky.frag changes materially, mirror it here.

layout(set = 0, binding = 0) uniform IblSkyUBO {
    vec4 sunDir;     // xyz = normalized direction TOWARD the sun
    vec4 sunColor;   // rgb = sun color, a = intensity
    vec4 params;     // x = haze, y = exposure, z = skyTime, w = enabled(>0.5)
    vec4 zenith;     // rgb = overhead sky color (linear)
    vec4 horizon;    // rgb = horizon glow color (linear)
} sky;

// Per-face cube basis: faceFwd points out of the face center; faceRight/faceUp
// span the face. dir = normalize(faceFwd + (2u-1)*right + (2v-1)*up).
layout(push_constant) uniform Push {
    vec4 faceFwd;
    vec4 faceRight;
    vec4 faceUp;
} pc;

layout(location = 0) in vec2 vUV;     // [0,1] across the face
layout(location = 0) out vec4 outColor;

float starHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123); }
vec3 starField(vec3 dir, float t) {
    float c = cos(t * 0.02), s = sin(t * 0.02);
    vec3 d = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
    vec2 uv = vec2(atan(d.z, d.x) * 0.1591549 + 0.5,
                   asin(clamp(d.y, -1.0, 1.0)) * 0.3183099 + 0.5);
    vec3 acc = vec3(0.0);
    for (int o = 0; o < 2; ++o) {
        float dens = (o == 0) ? 260.0 : 150.0;
        vec2 g = uv * dens;
        vec2 cell = floor(g);
        float h = starHash(cell + float(o) * 19.7);
        float present = step(0.955, h);
        vec2 f = fract(g) - 0.5;
        float pt = present * smoothstep(0.16, 0.0, length(f));
        float tw = 0.6 + 0.4 * sin(t * 2.0 + h * 31.4);
        float bright = (0.35 + 0.65 * starHash(cell + 7.1)) * tw;
        vec3 tint = mix(vec3(0.8, 0.85, 1.0), vec3(1.0, 0.9, 0.8), starHash(cell + 2.3));
        acc += pt * bright * tint * ((o == 0) ? 1.0 : 0.6);
    }
    return acc;
}

// Evaluate the analytic sky radiance along a world ray `dir` (mirrors sky.frag).
vec3 skyRadiance(vec3 dir) {
    vec3 sunDir = normalize(sky.sunDir.xyz);
    float haze     = clamp(sky.params.x, 0.0, 1.0);
    float exposure = max(sky.params.y, 0.0001);

    float up = dir.y;
    float t  = clamp(up, 0.0, 1.0);

    vec3 kZenith  = sky.zenith.rgb;
    vec3 kHorizon = sky.horizon.rgb;
    float grad = pow(t, 0.55);
    vec3 col = mix(kHorizon, kZenith, grad);

    float horizonBand = pow(1.0 - t, 8.0);
    vec3 hazeTint = clamp(kHorizon * 1.12, 0.0, 1.0);
    col = mix(col, hazeTint, horizonBand * (0.35 + 0.45 * haze));

    if (up > 0.0) {
        float skyLum = dot(col, vec3(0.299, 0.587, 0.114));
        float night  = pow(clamp(1.0 - skyLum, 0.0, 1.0), 4.0);
        col += starField(dir, sky.params.z) * (1.5 * night) * smoothstep(0.0, 0.12, up);
    }

    float cosAngle = clamp(dot(dir, sunDir), -1.0, 1.0);
    vec3 sunRGB = sky.sunColor.rgb;
    float sunI  = max(sky.sunColor.a, 0.0);
    // moon lane (zenith.w, W-NIGHT): at night the luminary is the MOON — no warm
    // Mie flare in the fill (the disc itself is angularly tiny; its energy is
    // nothing at irradiance scale, so it is simply dropped here).
    float moonW = clamp(sky.zenith.w, 0.0, 1.0);
    float glow = pow(max(cosAngle, 0.0), 8.0)  * 0.20
               + pow(max(cosAngle, 0.0), 256.0) * 0.55;
    col += sunRGB * glow * sunI * (1.0 - moonW);
    // Sun disk: SOFTENED + energy-capped for IBL. The razor-thin disk in sky.frag
    // (4*sunI over ~0.95deg) would, prefiltered, dump a hot speckle; widen + scale
    // down so the prefilter integrates a clean broad highlight instead.
    float diskInner = 0.9990;
    float diskOuter = 0.9965;
    float disk = smoothstep(diskOuter, diskInner, cosAngle);
    col += sunRGB * disk * (2.0 * sunI) * (1.0 - moonW);

    if (up < 0.0) {
        // W-NIGHT: earth by skylight — scale the ground tint with the sky's own
        // horizon luminance (clamps to exactly 1.0 for the day palette, so every
        // existing bake is unchanged). Without this the night FILL kept a 0.15-grey
        // lower hemisphere — a phantom daylit ground lighting everything from below
        // while the sky above it was near-black.
        float hLum = dot(sky.horizon.rgb, vec3(0.299, 0.587, 0.114));
        vec3 ground = vec3(0.16, 0.15, 0.13) * clamp(hLum / 0.30, 0.05, 1.0);
        float belowT = clamp(-up * 4.0, 0.0, 1.0);
        col = mix(col, ground, belowT);
    }

    col *= exposure;
    return col;
}

void main() {
    // Map the [0,1] face UV to [-1,1] and build the world ray for this texel.
    vec2 fc = vUV * 2.0 - 1.0;
    vec3 dir = normalize(pc.faceFwd.xyz + fc.x * pc.faceRight.xyz + fc.y * pc.faceUp.xyz);
    outColor = vec4(skyRadiance(dir), 1.0);
}
