#version 450

// LENS FLARE + ANAMORPHIC STREAK generation (CLEAN-ROOM, original).
//
// The "Abrams / Star-Trek-Fleet-Command" cinematic flare. This pass runs AFTER
// bloom (so the bright-pass mip0 already isolates the HDR bright spots) and
// BEFORE the composite/tonemap — it writes a half-res HDR flare buffer that the
// composite ADDS into the linear scene color, so ACES rolls the added radiance
// off naturally (no white-out). Everything here is ENERGY-LIMITED and clamped:
// this codebase has a white-out history, so the defaults are restrained.
//
// References (technique, not source): Hullin et al. "Physically-Based Real-Time
// Lens Flare Rendering" (SIGGRAPH 2011) for the ghost/halo idea; the classic
// screen-space "John Chapman" procedural-flare writeup for the toward-center
// ghost chain + chromatic aberration + halo. NO engine source consulted.
//
// Inputs:
//   set0/binding0 = bloomTex : the bloom bright-pass mip0 (half-res HDR; bright
//                              spots already isolated by the soft-knee threshold).
//   set0/binding1 = depthTex : full-res scene depth (NEAREST). Used to occlusion-
//                              test the sun's screen position for the hero flare.
//
// A small UBO carries the sun's projected screen position + occlusion the CPU
// pre-computed, plus all the cvar-driven tunables (intensity, streak, ghosts).

layout(set = 0, binding = 0) uniform sampler2D bloomTex;   // bright-pass mip0 (half res)
layout(set = 0, binding = 1) uniform sampler2D depthTex;   // full-res scene depth

layout(set = 0, binding = 2) uniform LensUBO {
    vec2  sunScreen;     // sun position in UV [0..1] (xy); valid only if sunValid > 0
    float sunValid;      // 1 = sun is on-screen this frame, 0 = not (skip the hero flare)
    float sunDepth;      // sun's clip-space depth [0..1] for the occlusion compare

    float intensity;     // r_lensflare_intensity : overall flare gain (restrained)
    float streak;        // r_lensflare_streak    : anamorphic horizontal streak strength
    float ghosts;        // r_lensflare_ghosts    : ghost count (1..8, used as int)
    float aspect;        // viewport aspect (w/h) so the halo/streak stay round/level

    vec3  streakTint;    // anamorphic streak tint (cool blue — the signature)
    float ghostDispersal;// spacing of the toward-center ghost chain
    float haloWidth;     // radius of the halo ring (toward center)
    float chroma;        // chromatic-aberration UV offset (per-channel) magnitude
    float sunSize;       // angular size of the procedural sun flare sprite/streak
    float maxRadiance;   // energy clamp on the generated flare (white-out guard)
} u;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

// Sample the bright-pass with a tiny chromatic split (the ghosts split RGB
// slightly so each ghost gets the rainbow-edge optical look).
vec3 sampleChroma(vec2 uv, float amount) {
    vec2 dir = (vec2(0.5) - uv);          // toward center
    vec3 c;
    c.r = texture(bloomTex, uv + dir * amount * 1.0).r;
    c.g = texture(bloomTex, uv + dir * amount * 2.0).g;
    c.b = texture(bloomTex, uv + dir * amount * 3.0).b;
    return c;
}

// Radial falloff weight (1 at center, 0 at edge) raised to a power for a tight
// optical falloff. Used to fade ghosts that land far from screen center.
float radialFalloff(vec2 uv, float power) {
    float d = length(vec2(0.5) - uv) / 0.7071;   // 0 at center, ~1 at corner
    return pow(clamp(1.0 - d, 0.0, 1.0), power);
}

void main() {
    vec3 flare = vec3(0.0);

    // ---- 1. GHOST CHAIN -------------------------------------------------
    // Sample the bright-pass along the vector from this pixel TOWARD screen
    // center, at several scaled offsets — the classic lens-ghost reflection
    // chain. Each ghost gets a slight chromatic split + a radial falloff so the
    // chain fades toward the frame edge (where real optical ghosts vanish).
    vec2 texelToCenter = (vec2(0.5) - vUV) * u.ghostDispersal;
    int  ng = int(clamp(u.ghosts, 0.0, 8.0));
    for (int i = 0; i < ng; ++i) {
        vec2 offset = texelToCenter * float(i);
        vec2 guv = vUV + offset;
        // Mirror UVs that walk off-screen back in (ghosts can land past center).
        guv = clamp(guv, vec2(0.0), vec2(1.0));
        vec3 g = sampleChroma(guv, u.chroma);
        float w = radialFalloff(guv, 3.0);   // tighter optical falloff -> less wash
        // Later ghosts are dimmer (the reflection loses energy each bounce).
        float bounce = 1.0 - float(i) / float(max(ng, 1));
        flare += g * w * bounce * 0.7;        // restrain the ghost-chain energy
    }

    // ---- 2. HALO --------------------------------------------------------
    // A ring sampled at a fixed distance toward center (the wide soft halo a
    // bright source throws around itself). The halo direction is normalized so
    // it forms a ring rather than tracking the ghost line.
    vec2 haloDir = normalize(vec2(0.5) - vUV + 1e-5) * u.haloWidth;
    // Correct for aspect so the halo reads circular, not elliptical.
    haloDir.x /= max(u.aspect, 1e-3);
    vec2 huv = clamp(vUV + haloDir, vec2(0.0), vec2(1.0));
    float haloW = radialFalloff(huv, 4.0);
    flare += sampleChroma(huv, u.chroma * 1.5) * haloW * 0.35;

    // ---- 3. ANAMORPHIC STREAK (the signature) ---------------------------
    // A wide separable HORIZONTAL gaussian smear of the bright-pass — the blue
    // horizontal light-streak across bright sources. Blue-tinted + additive.
    // 17 taps across a wide horizontal kernel; the weight is a gaussian so the
    // streak tapers smoothly. cvar-controlled width via u.streak.
    if (u.streak > 0.0) {
        vec3 streakAccum = vec3(0.0);
        float wsum = 0.0;
        const int   N = 8;          // +/- 8 taps -> 17 total
        float spread = 0.012 + u.streak * 0.02;   // half-width in UV
        for (int i = -N; i <= N; ++i) {
            float fi = float(i) / float(N);
            float w = exp(-fi * fi * 3.0);         // gaussian
            vec2 suv = vUV + vec2(fi * spread, 0.0);
            suv = clamp(suv, vec2(0.0), vec2(1.0));
            streakAccum += texture(bloomTex, suv).rgb * w;
            wsum += w;
        }
        streakAccum /= max(wsum, 1e-4);
        flare += streakAccum * u.streakTint * u.streak;
    }

    // ---- 4. OCCLUSION-TESTED SUN FLARE (the hero element) --------------
    // The sun's screen position came from the CPU (SkyParams.sunDir projected).
    // Soft-test the depth buffer around that point: if scene geometry is nearer
    // than the sun, the flare is occluded. We average a small kernel so the
    // flare fades GRADUALLY as the capital ship's hull slides across the sun
    // (rather than popping off) — this is what makes the eclipse shot sing.
    if (u.sunValid > 0.5) {
        // Occlusion fraction: sample a small kernel of depth around the sun and
        // count how many taps have geometry IN FRONT of the sun's depth.
        float vis = 0.0;
        const int   K = 2;     // 5x5 kernel
        const float kstep = 0.004;
        for (int y = -K; y <= K; ++y)
        for (int x = -K; x <= K; ++x) {
            vec2 duv = u.sunScreen + vec2(float(x), float(y)) * kstep;
            duv = clamp(duv, vec2(0.0), vec2(1.0));
            float d = texture(depthTex, duv).r;
            // Sky is at the far plane (depth ~= 1.0). The sun "passes" the test
            // (is visible) wherever the depth is essentially the far plane OR
            // farther than the sun's own depth.
            vis += (d >= u.sunDepth - 1e-4) ? 1.0 : 0.0;
        }
        vis /= float((2 * K + 1) * (2 * K + 1));

        if (vis > 0.0) {
            // Distance from this pixel to the sun (aspect-corrected so the disc
            // and rays are round/level, not stretched).
            vec2 d = vUV - u.sunScreen;
            d.x *= u.aspect;
            float dist = length(d);

            // Bright core disc + soft glow (kept tight so the sun reads as a
            // crisp disc with a halo, not a frame-filling wash).
            float core = exp(-dist * dist / (u.sunSize * u.sunSize * 0.02));
            float glow = exp(-dist / (u.sunSize * 0.45)) * 0.35;

            // A strong HORIZONTAL streak anchored at the sun (the blue ray).
            float horiz = exp(-(d.y * d.y) / (u.sunSize * u.sunSize * 0.0008));
            horiz *= exp(-abs(d.x) / (u.sunSize * 3.0));

            // Subtle vertical ray + a faint 45-degree cross for a star glint.
            float vert = exp(-(d.x * d.x) / (u.sunSize * u.sunSize * 0.0025))
                       * exp(-abs(d.y) / (u.sunSize * 1.6)) * 0.4;

            // Fade by occlusion fraction + distance from the frame edge (real
            // flares dim as their source nears the edge of the lens).
            float edgeFade = radialFalloff(u.sunScreen, 1.0);
            float sun = (core + glow) * 1.0;
            vec3  sunColor = vec3(1.0, 0.93, 0.82);   // warm core
            vec3  hero = sunColor * sun
                       + u.streakTint * (horiz + vert) * max(u.streak, 0.4);
            flare += hero * vis * edgeFade;
        }
    }

    // ---- 5. ENERGY LIMIT + GAIN ----------------------------------------
    // Clamp the generated radiance (white-out guard) THEN apply the user gain.
    // The composite adds this into linear HDR before ACES, so a modest value
    // here blooms nicely without blowing the highlights flat.
    flare = min(flare, vec3(u.maxRadiance));
    flare *= u.intensity;

    outColor = vec4(flare, 1.0);
}
