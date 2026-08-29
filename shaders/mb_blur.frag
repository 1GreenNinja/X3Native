#version 450

// MOTION BLUR — STAGE 3 of 3: RECONSTRUCTION FILTER.  (CLEAN-ROOM, original.)
//
// A real camera integrates light over a finite exposure.  Anything that moves
// while the shutter is open deposits energy along the path it travelled, not at
// a point, so a film frame is a LINE INTEGRAL of the scene over time.  A
// renderer that samples one instant produces frames that are each individually
// too correct; played back, the eye reads the sequence of sharp, temporally
// uncorrelated samples as strobing.  This pass reconstructs the line integral
// from one colour sample plus a velocity field.
//
// WHERE IT RUNS: after taa-history-copy, before auto-exposure.  After the
// history copy so the blurred frame never enters the TAA history (a blurred
// history feeds back and smears permanently); before bloom so the bloom chain
// blooms the blurred image, which is what a real lens does.
//
// THE FILTER.  For each pixel X we do NOT simply walk backwards along X's own
// velocity -- that leaves every silhouette hard-edged (see mb_tilemax.frag).
// Instead we walk along vN, the largest velocity in X's tile neighbourhood
// (stages 1-2), and decide per TAP whether that tap is a plausible contributor,
// using depth and the tap's own speed:
//
//   * a tap Y in FRONT of X, moving fast enough to have covered the distance
//     |Y-X| during the exposure, swept ACROSS X and contributes  -> cone(|Y-X|, |vY|)
//   * a tap Y BEHIND X contributes only in so far as X ITSELF is moving and
//     therefore gathered light from behind it                    -> cone(|Y-X|, |vX|)
//   * two taps at comparable speed and comparable depth blur into each other
//     mutually                                                   -> cylinder x cylinder
//
// That asymmetry is the depth ordering.  Its two consequences are the ones worth
// stating, because they are the artefacts cheap motion blur is known for:
//   - a STATIC foreground object in front of a fast background receives nothing
//     from that background (fg = 0 and cone(d, |vX| ~ 0) = 0), so it stays sharp
//     instead of being painted with background colour;
//   - a SHARP foreground object does not smear onto a moving background either
//     (cone(d, |vY| ~ 0) = 0), so silhouettes do not double.
//
// STATIC-CAMERA IDENTITY.  When nothing moves, every velocity is zero, vN is
// zero, and the early-out below returns the centre texel untouched.  The pass is
// then EXACTLY the identity function.  That is not a happy accident, it is the
// property the motion-domain rig's negative control asserts: a still frame from
// a still camera must be bit-identical with the pass on and off.
//
// WHERE VELOCITY IS ABSENT.  The velocity pre-pass rasterizes OPAQUE draws only.
// Sky/no-geometry pixels are handled explicitly (camera-only reconstruction
// below).  Transparency, particles, debris, water and glass composite AFTER the
// velocity pass, so they inherit the velocity of whatever opaque surface is
// behind them -- an approximation, documented, and the subject of a separate
// velocity-coverage delta.  It degrades toward "blurred with the background"
// rather than toward garbage.
//
// References: public motion-blur literature -- McGuire, Hennessy, Bukowski and
// Osman, "A Reconstruction Filter for Plausible Motion Blur" (I3D 2012);
// Real-Time Rendering 4th ed. 12.5.  No game-engine source was consulted or
// copied; see PROVENANCE.md.

const int kTile = 20;   // must equal x3::rhi::kMotionBlurTile

layout(set = 0, binding = 0) uniform sampler2D colorTex;     // HDR scene (TAA out, or raw HDR)
layout(set = 0, binding = 1) uniform sampler2D velTex;       // RG16F, prevUV - curUV
layout(set = 0, binding = 2) uniform sampler2D depthTex;     // D32F, standard Z
layout(set = 0, binding = 3) uniform sampler2D neighborTex;  // RG16F neighbour-max, pixels

layout(set = 0, binding = 4) uniform MbUBO {
    mat4  invViewProjCur;
    mat4  viewProjPrev;
    vec4  params0;   // x,y = 1/extent   z = velocityScale   w = maxBlurPixels
    vec4  params1;   // x = sampleCount  y = ditherPhase     z = velocityValid  w = softZ (relative)
    vec4  params2;   // x,y = extent px  z = zLinA (P[2][2]) w = zLinB (P[3][2])
    vec4  params3;   // x,y = jitter-delta correction (UV)   z,w = unused
} mb;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

// Standard-Z nonlinear depth -> linear view distance in world units.
// For a GLM zero-to-one RH projection: d = (A*z + B)/(-z)  =>  -z = B/(d + A),
// with A = P[2][2], B = P[3][2].  Verified at both ends: d=0 -> near, d=1 -> far.
float linearDepth(float d) {
    return mb.params2.w / max(d + mb.params2.z, 1e-6);
}

vec2 cameraMotionUV(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 wp  = mb.invViewProjCur * ndc;
    if (abs(wp.w) < 1e-8) return vec2(0.0);
    wp /= wp.w;
    vec4 pc = mb.viewProjPrev * wp;
    if (pc.w <= 1e-8) return vec2(0.0);
    return ((pc.xy / pc.w) * 0.5 + 0.5) - uv;
}

// Velocity at a UV in dt-normalised PIXELS, with the sky substitution applied.
vec2 velocityPixels(vec2 uv, float depth, vec2 extent) {
    // + mb.params3.xy undoes the velocity pass' jitter over-subtraction, so a
    // static camera reads EXACTLY zero motion (see vk_passes.cpp's VelUBO fill;
    // without it a still frame carries ~1 px of spurious jitter velocity and the
    // identity guarantee below is only approximate). Zero when jitter is off.
    vec2 v = texture(velTex, uv).rg + mb.params3.xy;
    if (depth >= 0.999999) v = cameraMotionUV(uv, depth);
    vec2  px  = v * mb.params0.z * extent;
    float len = length(px);
    if (len > mb.params0.w) px *= mb.params0.w / max(len, 1e-6);
    return px;
}

// Would a thing of blur length r, centred d pixels away, still reach here?
float cone(float d, float r)     { return clamp(1.0 - d / max(r, 1e-4), 0.0, 1.0); }
// Are two blur lengths comparable enough that the taps blur into each other?
float cylinder(float d, float r) { return 1.0 - smoothstep(0.95 * r, 1.05 * r, d); }

// Interleaved-gradient noise: decorrelates the tap positions between neighbouring
// pixels so an under-sampled blur reads as fine grain instead of hard banding.
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    const vec2  extent = mb.params2.xy;
    const vec2  texel  = mb.params0.xy;
    const int   S      = int(mb.params1.x);
    const float softZ  = mb.params1.w;

    // texelFetch, NOT texture(): the early-out below must be EXACTLY the identity
    // function, and a filtered fetch is not. vUV lands on the texel centre in
    // theory, but barycentric interpolation puts it a few ULPs off in practice,
    // so a bilinear tap leaks a ~1e-7 weight from a neighbour -- enough to flip
    // the odd 8-bit value after tonemapping and to break the static-camera
    // bit-identity claim the rig asserts. Measured: it did.
    const vec3 centerColor = texelFetch(colorTex, ivec2(gl_FragCoord.xy), 0).rgb;

    // Neighbourhood dominant velocity for this pixel's tile.  Sampled at the tile
    // centre (NEAREST on a grid-sized target) so every pixel in a tile agrees.
    const vec2 gridUV = (floor(gl_FragCoord.xy / float(kTile)) + 0.5)
                      / vec2(textureSize(neighborTex, 0));
    const vec2 vN     = texture(neighborTex, gridUV).rg;
    const float lenN  = length(vN);

    // NOTHING NEAR THIS PIXEL MOVED -> IDENTITY, bit for bit.  This is the
    // static-camera negative control, enforced in the shader rather than asserted
    // in a doc: with the fetch above being exact, a still frame from a still
    // camera comes out of this pass byte-identical to the way it went in.
    if (lenN < 0.5) { outColor = vec4(centerColor, 1.0); return; }

    const float depthX = texture(depthTex, vUV).r;
    const float zX     = linearDepth(depthX);
    const vec2  vX     = velocityPixels(vUV, depthX, extent);
    const float lenX   = max(length(vX), 0.5);

    // Per-pixel jitter of the tap positions.  params1.y is 0 on headless frames
    // (bit-reproducible captures) and cycles otherwise, so the residual grain is
    // integrated away over time in an interactive session.
    const float jitter = ign(gl_FragCoord.xy + mb.params1.y) - 0.5;

    // The centre tap is weighted as one of S so a barely-moving pixel is not
    // dominated by its neighbours' motion.
    float wsum = 1.0 / float(S);
    vec3  sum  = centerColor * wsum;

    for (int i = 0; i < S; ++i) {
        // Alternate between the neighbourhood vector (bleed from fast neighbours,
        // the silhouette fix) and this pixel's own vector (self-blur).
        const vec2 dir = ((i & 1) == 0) ? vN : vX;
        // t in (-0.5, 0.5): the exposure interval centred on the sampled instant.
        const float t = (float(i) + 0.5 + jitter) / float(S) - 0.5;

        const vec2 offsetPx = dir * t;
        const vec2 uvY      = vUV + offsetPx * texel;
        if (any(lessThan(uvY, vec2(0.0))) || any(greaterThan(uvY, vec2(1.0)))) continue;

        const float dist   = length(offsetPx);
        const float depthY = texture(depthTex, uvY).r;
        const float zY     = linearDepth(depthY);
        const vec2  vY     = velocityPixels(uvY, depthY, extent);
        const float lenY   = max(length(vY), 0.5);

        // Relative (scale-invariant) soft depth compare: fg -> 1 when Y is in
        // front of X, bg -> 1 when Y is behind it, with a soft band of softZ
        // times X's own distance so co-planar taps score as neither.
        const float band = max(softZ * zX, 1e-4);
        const float fg   = clamp(1.0 - (zY - zX) / band, 0.0, 1.0);
        const float bg   = clamp(1.0 - (zX - zY) / band, 0.0, 1.0);

        const float w = fg * cone(dist, lenY)
                      + bg * cone(dist, lenX)
                      + 2.0 * cylinder(dist, lenY) * cylinder(dist, lenX);

        sum  += w * texture(colorTex, uvY).rgb;
        wsum += w;
    }

    vec3 result = sum / max(wsum, 1e-6);
    if (any(isnan(result))) result = centerColor;   // never poison the post chain
    outColor = vec4(max(result, vec3(0.0)), 1.0);
}
