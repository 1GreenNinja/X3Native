#ifndef X3_MESH_SHADOWS_GLSL
#define X3_MESH_SHADOWS_GLSL
#ifdef RT_SHADOWS
// ===========================================================================
// RAY-TRACED SOFT SHADOWS (r_rtshadows) — per-pixel inline ray queries.
//   * SUN (tier >= 1): ONE shadow ray per pixel toward the sun, cone-jittered
//     by the sun's angular radius (rtsh0.y = tan(radius); ~0.5 deg default) —
//     contact-hardening penumbra. Combined min() with the CSM term so DYNAMIC
//     (skinned) casters — absent from the static TLAS — keep their raster
//     shadows.
//   * POINT LIGHTS (tier >= 2): in the light loop, the first K lights with a
//     non-negligible contribution at this pixel each get ONE shadow ray toward
//     a jittered point on the light's spherical source (radius rtsh0.w);
//     penumbra widens with occluder->receiver distance by construction.
//     Beyond K (rtsh0.z) or below the contribution floor: unshadowed (the
//     existing behavior).
// Per-frame jitter rotation (rtsh1.x seed) turns the 1-spp penumbra noise into
// temporal samples TAA accumulates away; with TAA off the host pins the seed
// so the dither is STATIC (no sizzle).
// DOCUMENTED v1 LIMITS (shared with RT-AO/reflections/DDGI — same TLAS):
//   * opaque-only rays: alpha-cutout surfaces (foliage/billboards) occlude as
//     their full quad; * skinned characters don't cast (CSM keeps the sun's).
// ===========================================================================
uint rtshWang(uint s) {
    s = (s ^ 61u) ^ (s >> 16); s *= 9u; s = s ^ (s >> 4); s *= 0x27d4eb2du; s = s ^ (s >> 15);
    return s;
}
float rtshRnd(inout uint st) { st = rtshWang(st); return float(st & 0x00FFFFFFu) / float(0x01000000u); }

// Orthonormal basis around a unit vector (the rtao.comp pattern).
void rtshBasis(vec3 N, out vec3 T, out vec3 B) {
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// Binary visibility: 1 = the segment origin->origin+dir*tMax is unobstructed.
// Opaque-only + terminate-on-first-hit: one proceed resolves it (rtao pattern).
// cullMask 0x80: GLASS TLAS instances carry mask 0x7F (bit 7 clear — see the
// TLAS build), so shadow rays pass through glass — the sun reaches the world
// under the WATER surface (the underwater direct term / caustics) and through
// panes, exactly like the raster CSM (which draws only the opaque range).
// Every other RT consumer (AO/reflections/DDGI) keeps cullMask 0xFF.
float rtshVisibility(vec3 origin, vec3 dir, float tMax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, rtShadowTlas,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
        0x80, origin, 0.01, dir, tMax);
    rayQueryProceedEXT(rq);
    return (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        ? 1.0 : 0.0;
}

// Sun visibility: one cone-jittered ray toward the sun. tanRadius = tan of the
// sun's angular radius; the jitter disk is perpendicular to the sun direction.
float rtshSunVisibility(vec3 P, vec3 Ng, vec3 sunDir, inout uint seed) {
    vec3 T, B; rtshBasis(sunDir, T, B);
    float r   = sqrt(rtshRnd(seed)) * ssao.rtsh0.y;     // uniform disk * tan(radius)
    float phi = 6.2831853 * rtshRnd(seed);
    vec3 dir = normalize(sunDir + (T * cos(phi) + B * sin(phi)) * r);
    return rtshVisibility(P + Ng * 0.03, dir, 500.0);
}

// Point-light visibility: one ray toward a jittered point on the light's
// spherical source. tMax stops SHORT of the source (clearance = max(lightR,
// 0.12 m)) so a light parked inside its own fixture mesh doesn't self-occlude.
float rtshPointVisibility(vec3 P, vec3 Ng, vec3 toL, float dist, inout uint seed) {
    float lr = ssao.rtsh0.w;
    vec3 L = toL / max(dist, 1e-4);
    vec3 T, B; rtshBasis(L, T, B);
    float r   = sqrt(rtshRnd(seed)) * lr;
    float phi = 6.2831853 * rtshRnd(seed);
    vec3 origin = P + Ng * 0.03;
    vec3 seg = (P + toL + (T * cos(phi) + B * sin(phi)) * r) - origin;
    float len = length(seg);
    float tMax = len - max(lr, 0.12);
    if (tMax <= 0.02) return 1.0;                        // too close to the source
    // W6-1: near-source early-out. The 0.12 m clearance is smaller than typical
    // FIXTURE geometry around a light, so surfaces near the lamp traced rays that
    // clipped the fixture's own corners -> the black speckle RING around emitters
    // (AD-2 survey, lamp_rtshadows_on). Within half a metre of a point light the
    // surface counts lit — a light's own housing must not shadow its surroundings.
    if (len < max(lr, 0.12) + 0.45) return 1.0;
    return rtshVisibility(origin, seg / max(len, 1e-4), tMax);
}
#endif

// ===========================================================================
// CASCADED SHADOW MAPS (r_csm) - cascade selection, blend band, per-cascade bias.
// Clean-room, original work: practical/parallel-split shadow maps (Zhang et al.
// 2006) plus the standard stable-CSM technique. The CPU-side fitting lives in
// engine/rhi/Csm.h and is asserted by --test-csm. No id Tech / RBDOOM source
// consulted.
//
// The shadow map is a 2D ARRAY, one layer per cascade. Layer 0 is the LEGACY
// cascade, so r_csm 0 samples exactly the texels the single-map renderer wrote.
// ===========================================================================

// 3x3 PCF in ONE cascade layer: average 9 hardware-compare taps a texel apart.
// Returns the lit fraction (1 = fully lit, 0 = fully shadowed). Outside this
// cascade's frustum it returns fully lit, which lets the caller fall through to
// a coarser cascade instead of stamping a hard black edge.
float csmPcf(int layer, vec3 worldPos, float bias) {
    vec4 lc = csm.viewProj[layer] * vec4(worldPos, 1.0);
    vec3 proj = lc.xyz / lc.w;                  // ortho => w==1, but stay general
    // XY: clip [-1,1] -> UV [0,1]. Z is already [0,1] (GLM_FORCE_DEPTH_ZERO_TO_ONE).
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0)
        return 1.0;
    float refDepth = proj.z - bias;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += texture(shadowMap, vec4(uv + vec2(x, y) * texel, float(layer), refDepth));
    return lit / 9.0;
}

// Sun shadow visibility at `worldPos`. `N` is the shading normal (it drives the
// per-cascade NORMAL-OFFSET bias) and `ndl` = dot(N, L).
//
// TWO PATHS, and the first must never drift:
//   * csm.ctrl.x == 0 -> the LEGACY single ~45 m cascade, character-for-character
//     the pre-CSM math, sampling array layer 0 (which the shadow pass filled with
//     exactly the legacy matrix). This is what keeps r_csm 0 bit-exact for the
//     md5/screenshot gates.
//   * csm.ctrl.x  > 0 -> cascade selection by VIEW depth, a smoothstep blend band
//     across each split so transitions are a gradient not a visible line, and
//     per-cascade depth + normal-offset bias.
float sampleShadow(vec3 worldPos, vec3 N, float ndl) {
    int cascadeCount = int(csm.ctrl.x);

    if (cascadeCount <= 0) {
        // ---- LEGACY PATH (r_csm 0) - do not "improve" this branch ------------
        // Slope-scaled depth bias, clamped, in light-clip depth units: grazing
        // surfaces (where the depth slope is steep) must not self-shadow (acne),
        // while a steep bias on flat faces must not cause peter-panning.
        vec4 lc = cam.lightViewProj * vec4(worldPos, 1.0);
        vec3 proj = lc.xyz / lc.w;
        vec2 uv = proj.xy * 0.5 + 0.5;
        float curDepth = proj.z;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || curDepth > 1.0)
            return 1.0;
        float bias = clamp(0.0015 * tan(acos(clamp(ndl, 0.0, 1.0))), 0.0005, 0.004);
        float refDepth = curDepth - bias;
        vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0).xy);
        float lit = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                lit += texture(shadowMap, vec4(uv + vec2(x, y) * texel, 0.0, refDepth));
        return lit / 9.0;
    }

    // ---- CASCADED PATH ---------------------------------------------------
    // View depth = the perspective clip w (the projection is RH with
    // GLM_FORCE_DEPTH_ZERO_TO_ONE, so w == -viewZ == distance along the view
    // axis). Exact, and cheaper than reconstructing the camera basis here.
    float viewDepth = (cam.viewProj * vec4(worldPos, 1.0)).w;

    int layer = cascadeCount - 1;
    for (int i = 0; i < cascadeCount; ++i) {
        if (viewDepth < csm.splitFar[i]) { layer = i; break; }
    }

    // Per-cascade bias. The NORMAL OFFSET is the important half: it pushes the
    // sample point off the surface by ~1.5 of THIS cascade's texels, which is
    // the real scale of the error a depth compare has to absorb. One constant
    // bias cannot serve cascade 0 and the last cascade at once - their texels
    // differ by more than an order of magnitude - so tuning for one acnes or
    // peter-pans the other. Slope scaling on top: grazing light needs more.
    float slope  = clamp(1.0 - ndl, 0.0, 1.0);
    vec3  offPos = worldPos + N * (csm.normalBias[layer] * (1.0 + 2.0 * slope));
    float bias   = csm.depthBias[layer] * (1.0 + 2.0 * slope);

    float lit = csmPcf(layer, offPos, bias);

    // BLEND BAND: near the outer edge of this cascade, cross-fade into the next.
    // Without it the resolution change lands as a hard line across the ground.
    // The band is a fraction (ctrl.y) of the cascade's far distance.
    if (layer + 1 < cascadeCount && csm.ctrl.y > 0.0) {
        float farD = csm.splitFar[layer];
        float band = farD * csm.ctrl.y;
        float t = smoothstep(farD - band, farD, viewDepth);
        if (t > 0.0) {
            int   nx      = layer + 1;
            vec3  offNext = worldPos + N * (csm.normalBias[nx] * (1.0 + 2.0 * slope));
            lit = mix(lit, csmPcf(nx, offNext, csm.depthBias[nx] * (1.0 + 2.0 * slope)), t);
        }
    }
    return lit;
}
#endif  // X3_MESH_SHADOWS_GLSL
