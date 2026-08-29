#version 450

// MOTION BLUR — STAGE 1 of 3: TILE MAX velocity reduction.  (CLEAN-ROOM, original.)
//
// Reduces the full-resolution RG16F velocity buffer to a coarse grid where each
// texel holds the LARGEST motion vector found inside one kTile x kTile block of
// screen pixels, expressed in dt-normalised PIXELS.
//
// WHY THIS EXISTS (the whole reason motion blur is not one pass):
// a naive blur gathers each pixel backwards along ITS OWN velocity.  That is
// wrong at every silhouette, and silhouettes are exactly where the eye looks.
// Take a fast car against a static wall.  A wall pixel just ahead of the car has
// ZERO velocity, so a naive filter leaves it perfectly sharp -- yet physically
// the car swept ACROSS that pixel during the exposure and must deposit some of
// its colour there.  A static pixel therefore has to know about its fast
// neighbours.  Tile-max (here) plus neighbour-max (stage 2) give every pixel a
// cheap conservative answer to "what is the fastest thing near me that could
// have swept over me?".  Without it you get a hard-edged sharp silhouette around
// a blurred interior, which reads worse than no blur at all.
//
// UNITS / THE dt RULE.  shaders/velocity.frag writes (prevUV - curUV): a
// per-FRAME displacement in UV space.  A per-frame delta is NOT a physical
// speed -- at 165 Hz it is ~1/5 of what it is at 33 Hz for the same real motion.
// Scaling blur by it directly would make the effect vanish at high framerate and
// overwhelm at low framerate, i.e. STRONGEST exactly when the machine is
// struggling, which is backwards.  So the conversion to pixels folds in
//     velocityScale = shutter * (referenceFrameTime / dt)
// computed once CPU-side by x3::rhi::motionBlurVelocityScale() and delivered in
// mb.params0.z.  Applied HERE, once, so stages 2 and 3 work in one consistent
// unit and the rule lives in exactly one place.
//
// SKY / NO-GEOMETRY.  The velocity attachment is CLEARed to 0, so pixels the
// velocity pre-pass never rasterized (sky, and everything drawn after the opaque
// pass) read as motionless.  For a rotating camera the sky is emphatically NOT
// motionless, so where depth says "no geometry" we substitute a camera-only
// motion vector reconstructed from the current/previous view-projections -- the
// same reconstruction taa_resolve.frag uses for its fallback path.
//
// References: public motion-blur literature -- McGuire, Hennessy, Bukowski and
// Osman, "A Reconstruction Filter for Plausible Motion Blur" (I3D 2012) and the
// tile-velocity dilation it describes; Real-Time Rendering 4th ed. 12.5.  No
// game-engine source was consulted or copied; see PROVENANCE.md.

// MUST equal x3::rhi::kMotionBlurTile (engine/rhi/MotionBlur.h).  A plain const,
// not a specialization constant, so there is exactly one number to keep in sync
// and no VkSpecializationInfo plumbing to get wrong.
const int kTile = 20;

layout(set = 0, binding = 0) uniform sampler2D velTex;    // RG16F, prevUV - curUV
layout(set = 0, binding = 1) uniform sampler2D depthTex;  // D32F, standard Z (near=0, far=1)

layout(set = 0, binding = 2) uniform MbUBO {
    mat4  invViewProjCur;  // current clip -> world (matches the depth buffer)
    mat4  viewProjPrev;    // world -> previous unjittered clip
    vec4  params0;         // x,y = 1/extent   z = velocityScale   w = maxBlurPixels
    vec4  params1;         // x = sampleCount  y = ditherPhase     z = velocityValid  w = softZ
    vec4  params2;         // x,y = extent px  z = zLinA (P[2][2]) w = zLinB (P[3][2])
} mb;

layout(location = 0) out vec2 outTileMax;   // pixels, dt-normalised

// Camera-only screen motion for a pixel with no rasterized velocity.  Same
// (prevUV - curUV) convention as the velocity buffer, in UV space.
vec2 cameraMotionUV(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 wp  = mb.invViewProjCur * ndc;
    if (abs(wp.w) < 1e-8) return vec2(0.0);
    wp /= wp.w;
    vec4 pc = mb.viewProjPrev * wp;
    if (pc.w <= 1e-8) return vec2(0.0);
    return ((pc.xy / pc.w) * 0.5 + 0.5) - uv;
}

void main() {
    const ivec2 extent = ivec2(mb.params2.xy);
    const vec2  texel  = mb.params0.xy;
    const float scale  = mb.params0.z;
    const float maxPx  = mb.params0.w;

    const ivec2 tileOrigin = ivec2(gl_FragCoord.xy) * kTile;

    vec2  best    = vec2(0.0);
    float bestLen = -1.0;

    // Exact reduction over the whole tile (kTile^2 texelFetches on an RG16F
    // target; the output grid is 1/kTile^2 of the screen, so the TOTAL work is
    // one fullscreen pass' worth of taps).  Not subsampled: a thin fast object
    // one pixel wide is precisely the case tile-max exists to catch.
    for (int y = 0; y < kTile; ++y) {
        for (int x = 0; x < kTile; ++x) {
            ivec2 p = tileOrigin + ivec2(x, y);
            if (p.x >= extent.x || p.y >= extent.y) continue;

            vec2  px  = texelFetch(velTex, p, 0).rg * scale * vec2(extent);
            float len = length(px);
            if (len > maxPx) { px *= maxPx / max(len, 1e-6); len = maxPx; }
            if (len > bestLen) { bestLen = len; best = px; }
        }
    }

    // Sky floor: one depth tap at the tile centre.  If this tile is
    // no-geometry (cleared depth) its velocity texels are the CLEAR value, not a
    // measurement, and a rotating camera must still blur the sky.  Substitute a
    // camera-only vector and let it compete with the measured max.
    {
        ivec2 c = min(tileOrigin + ivec2(kTile / 2), extent - 1);
        if (texelFetch(depthTex, c, 0).r >= 0.999999) {
            vec2  uv  = (vec2(c) + 0.5) * texel;
            vec2  px  = cameraMotionUV(uv, 1.0) * scale * vec2(extent);
            float len = length(px);
            if (len > maxPx) { px *= maxPx / max(len, 1e-6); len = maxPx; }
            if (len > bestLen) { bestLen = len; best = px; }
        }
    }

    outTileMax = best;
}
