#version 450

// TEMPORAL ANTI-ALIASING resolve (CLEAN-ROOM, original).
//
// The scene is rasterized each frame with a sub-pixel Halton(2,3) jitter folded
// into the projection matrix (8-frame cycle). This pass blends the jittered
// current frame with the accumulated HISTORY image, converging on a supersampled
// result over ~8 frames: stair-step edges resolve to smooth gradients while the
// image stays full-rate dynamic (no MSAA memory/bandwidth cost).
//
// Reprojection is CAMERA-ONLY (same model as ssgi_temporal.frag): reconstruct
// the pixel's world position from the current depth + the inverse CURRENT
// (jittered) viewProj — the exact matrices the meshes rasterized with — then
// project it with the PREVIOUS frame's UNJITTERED viewProj to find where this
// surface was last frame, and sample the history there (9-tap Catmull-Rom, so
// repeated resampling does not progressively blur the history).
//
// Ghosting containment: the history sample is CLAMPED to the YCoCg min/max of
// the current frame's 3x3 neighborhood, so history that no longer matches the
// scene (disocclusion, moving objects under camera-only reprojection) is pulled
// to a plausible current color instead of smearing. Reprojection uses the
// CLOSEST depth in the 3x3 neighborhood (depth dilation) so thin silhouettes
// reproject with the foreground surface they belong to.
//
// PER-OBJECT VELOCITY (#4): when a velocity buffer is present (params1.z > 0.5)
// the reprojection uses the per-pixel screen-space motion vector (binding 4)
// instead of camera-only reprojection. This tracks DYNAMIC + SKINNED motion
// (drone, monsters) directly — fast objects reproject onto their real previous
// location rather than relying on the neighborhood clamp (which traded their
// ghosting for shimmer). Camera-only reprojection remains the fallback for the
// background and for builds where the velocity pass is absent, so r_velocity 0
// (or a missing velocity.spv) is byte-identical to the pre-velocity TAA path.
//
// References: the public TAA literature (Karis SIGGRAPH 2014 talk notes,
// Real-Time Rendering 4th ed. §5.4.2, public temporal-AA + motion-vector
// write-ups). No game-engine source consulted.

layout(set = 0, binding = 0) uniform sampler2D sceneTex;  // current jittered HDR scene
layout(set = 0, binding = 1) uniform sampler2D histTex;   // accumulated history (HDR)
layout(set = 0, binding = 2) uniform sampler2D depthTex;  // current scene depth (NEAREST)
layout(set = 0, binding = 4) uniform sampler2D velTex;    // per-object MV (prevUV-curUV), RG16F

layout(set = 0, binding = 3) uniform TaaUBO {
    mat4 invViewProjCur;   // current JITTERED clip -> world (matches the depth buffer)
    mat4 viewProjPrev;     // world -> previous UNJITTERED clip
    vec4 params0;          // x,y = 1/extent (texel), z = historyValid (0/1), w = history blend (~0.9)
    vec4 params1;          // x,y = current jitter (pixels, debug), z = velocityValid (0/1), w = unused
} ub;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

// RGB <-> YCoCg: the neighborhood clamp in YCoCg is tighter on chroma than an
// RGB AABB, which suppresses color-fringe ghosting on high-contrast edges.
vec3 rgbToYcocg(vec3 c) {
    return vec3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                 0.5  * c.r              - 0.5  * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 ycocgToRgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// 9-tap bilinear-optimized Catmull-Rom: sharper history resampling than plain
// bilinear, so repeated reprojection doesn't accumulate blur frame over frame.
vec3 sampleHistoryCatmullRom(vec2 uv, vec2 texel) {
    vec2 samplePos = uv / texel;
    vec2 texPos1   = floor(samplePos - 0.5) + 0.5;
    vec2 f  = samplePos - texPos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 texPos0  = (texPos1 - 1.0) * texel;
    vec2 texPos3  = (texPos1 + 2.0) * texel;
    vec2 texPos12 = (texPos1 + offset12) * texel;
    vec3 result =
          texture(histTex, vec2(texPos0.x,  texPos0.y)).rgb  * w0.x  * w0.y
        + texture(histTex, vec2(texPos12.x, texPos0.y)).rgb  * w12.x * w0.y
        + texture(histTex, vec2(texPos3.x,  texPos0.y)).rgb  * w3.x  * w0.y
        + texture(histTex, vec2(texPos0.x,  texPos12.y)).rgb * w0.x  * w12.y
        + texture(histTex, vec2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y
        + texture(histTex, vec2(texPos3.x,  texPos12.y)).rgb * w3.x  * w12.y
        + texture(histTex, vec2(texPos0.x,  texPos3.y)).rgb  * w0.x  * w3.y
        + texture(histTex, vec2(texPos12.x, texPos3.y)).rgb  * w12.x * w3.y
        + texture(histTex, vec2(texPos3.x,  texPos3.y)).rgb  * w3.x  * w3.y;
    // Catmull-Rom weights can slightly undershoot below zero on hard edges.
    return max(result, vec3(0.0));
}

void main() {
    const vec2  texel     = ub.params0.xy;
    const bool  histValid = ub.params0.z > 0.5;
    const float blend     = ub.params0.w;

    // 3x3 neighborhood: current color + YCoCg min/max (clamp box) + the CLOSEST
    // depth (dilated reprojection so thin foreground edges track their surface).
    vec3  cur = vec3(0.0);
    vec3  cMin = vec3( 1e30);
    vec3  cMax = vec3(-1e30);
    float closestDepth = 1.0;
    vec2  closestOff   = vec2(0.0);   // texel offset of the closest-depth neighbor
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 off = vec2(float(dx), float(dy)) * texel;
            vec3 c   = texture(sceneTex, vUV + off).rgb;
            vec3 y   = rgbToYcocg(c);
            cMin = min(cMin, y);
            cMax = max(cMax, y);
            if (dx == 0 && dy == 0) cur = c;
            float d = texture(depthTex, vUV + off).r;
            if (d < closestDepth) { closestDepth = d; closestOff = off; }
        }
    }

    // First frame / camera cut / resize: no usable history -> passthrough.
    if (!histValid) { outColor = vec4(cur, 1.0); return; }

    const bool velValid = ub.params1.z > 0.5;

    vec2 prevUV;
    if (velValid) {
        // PER-OBJECT reprojection: sample the screen-space motion vector at the
        // CLOSEST-depth neighbor (depth dilation, so thin foreground silhouettes
        // track their own surface) and add it to this pixel's UV. The MV already
        // encodes dynamic + skinned + camera motion, jitter removed. A pixel the
        // velocity pass never wrote (sky / background gaps) has MV ~0, which
        // degrades to "same UV" — i.e. the static-background case, correct.
        vec2 mv = texture(velTex, vUV + closestOff).rg;
        prevUV = vUV + mv;
    } else {
        // Camera-only reprojection (fallback): world position from the dilated
        // depth via the CURRENT jittered inverse viewProj, then the PREVIOUS clip.
        // NDC xy uses vUV (NOT the closest neighbor) so this branch is
        // byte-identical to the pre-velocity TAA path when r_velocity 0.
        vec4 ndc = vec4(vUV * 2.0 - 1.0, closestDepth, 1.0);
        vec4 wp  = ub.invViewProjCur * ndc;
        if (abs(wp.w) < 1e-8) { outColor = vec4(cur, 1.0); return; }
        wp /= wp.w;
        vec4 prevClip = ub.viewProjPrev * wp;
        if (prevClip.w <= 0.0) { outColor = vec4(cur, 1.0); return; }
        prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    }

    // Off-screen last frame -> no history for this pixel.
    if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
        outColor = vec4(cur, 1.0); return;
    }

    // History sample (Catmull-Rom), clamped to the current neighborhood in YCoCg
    // to contain ghosting from disocclusion / unreprojected object motion.
    vec3 hist = sampleHistoryCatmullRom(prevUV, texel);
    hist = ycocgToRgb(clamp(rgbToYcocg(hist), cMin, cMax));

    // Exponential accumulation: keep `blend` of (clamped) history.
    vec3 result = mix(cur, hist, blend);

    // Paranoia: a NaN in history would otherwise poison the accumulator forever.
    if (any(isnan(result))) result = cur;
    outColor = vec4(max(result, vec3(0.0)), 1.0);
}
