// ===========================================================================
// mesh_wetness.glsl — SURFACE WETNESS (rain).            [LANE: inspx/wetness]
//
// This lane owns this file. mesh.frag calls applyWetness() once, between
// resolving the metallic-roughness material and building the BRDF terms, and
// nothing else in the frag touches wetness.
//
// WHAT A WATER FILM ACTUALLY DOES, and why each term is here:
//
//  1. THE DIFFUSE DARKENS. This is the effect everyone recognises and almost
//     nobody models correctly. Wet asphalt is not "asphalt multiplied by 0.5" —
//     it darkens because light refracts INTO the film, scatters around in the
//     substrate, and is partly trapped by total internal reflection at the
//     water/air boundary on the way back out. Rough, porous materials darken a
//     lot (concrete, cloth, soil); dense sealed ones barely at all (glass,
//     polished metal). We approximate the substrate dependence with the only
//     porosity signal a shader actually has for free: ROUGHNESS. A rough
//     surface has the open micro-structure that holds water, so it darkens
//     most, and a mirror-smooth one is nearly unchanged. That single coupling
//     is what stops wet glass from turning into a black hole.
//
//  2. THE SURFACE GETS SMOOTHER. Water fills the micro-cavities that made the
//     surface rough in the first place, so roughness collapses toward the
//     roughness of the film itself. This is the term that makes the street
//     mirror the city — it is what turns the existing reflection + DDGI work
//     into something you can see, rather than something in a changelog.
//
//  3. F0 RISES toward the air-water interface (~0.02..0.04 for the film over a
//     dielectric). Small, but it is why wet surfaces catch a hard specular
//     edge at grazing angles. Metals are excluded from all of this: a water
//     film over metal changes the interface, not the conductor's own F0, and
//     lerping a metal's tinted F0 toward grey desaturates chrome for no reason.
//
// WHERE THE WATER IS, per fragment. Rain is not a uniform coat:
//
//  * EXPOSURE — rain falls DOWN. An upward-facing face gets soaked, a vertical
//    wall gets a fraction, and a soffit, an overhang, or the inside of a tunnel
//    bore stays bone dry. That is just N.y, and it is the single term that most
//    makes this read as weather instead of as a global material slider. It also
//    means the terrain-corridor tunnels stay dry inside for free.
//
//  * POOLING — water collects where geometry dishes inward. Ambient occlusion
//    already measures exactly that (a cavity is occluded), so AO doubles as a
//    puddle mask at zero cost. Corners, gutters, kerb lines and the seams
//    between slabs wet up first, which is what a real street does.
//
// GATE: amount <= 0 returns before touching anything, so a dry world is
// byte-identical — no ALU, no divergence, nothing.
// ===========================================================================

// Roughness a fully-soaked surface converges toward when the host does not
// override it. Not zero: even standing water has ripples, and a perfect mirror
// reads as CGI rather than as a wet street.
const float kWetFilmRoughFallback = 0.06f;

// How far a vertical face gets wet relative to a horizontal one. Rain does
// blow onto walls, and runoff sheets down them, so this is not 0.
const float kWetVerticalFraction = 0.35f;

// What a NON-pooled water coat does to roughness: it smooths the substrate
// toward this fraction of its dry value, not to a mirror. Wet rock glistens;
// it does not reflect the skyline. Pooled water ignores this and levels to
// kWetFilmRough instead.
const float kWetCoatRoughScale = 0.55f;

// Perviousness of bare terrain (soil, grass, scree). Not zero — wet rock does
// catch some sheen — but nowhere near a paved surface, which is what produced
// the blown specular streaks across the cliff faces in the first capture.
const float kWetTerrainGloss = 0.18f;

// Ceiling on the diffuse darkening, reached only by a fully-soaked, fully
// porous surface. Measured wet/dry albedo ratios for concrete and asphalt sit
// around 0.4-0.6; 0.42 is the bottom of that range, and porosity scales it up.
const float kWetMaxDarken = 0.42f;

// Per-fragment wetness coverage in 0..1, before it is applied to anything.
// Split out from applyWetness so the self-test and the debug view can ask for
// the mask alone without also mutating a material.
//   N   : shading normal (world space, normalized)
//   ao  : ambient occlusion, 1 = open, 0 = fully occluded cavity
//   amount, puddles : from WetnessParams
// How much of the water here has POOLED (levelled out into standing water)
// rather than merely coating the surface. Drives roughness separately from
// coverage — see the note in applyWetness.
//
// OCCLUSION ALONE IS NOT A PUDDLE, which is the trap the first version fell
// into. AO is low on ANY occluded geometry, and a vertical cliff face shaded
// by its own terrain is heavily occluded — so reading AO as "cavity" laid
// mirror-flat standing water down a cliff and lit it with blown specular
// streaks under the noon sun. Water pools in an upward-facing DISH: it needs
// the cavity AND a surface close enough to level to hold it. Gravity is not
// optional.
float wetnessPooling(vec3 N, float ao, float puddles) {
    float level = smoothstep(0.55, 0.92, clamp(N.y, -1.0, 1.0));
    return clamp((1.0 - clamp(ao, 0.0, 1.0)) * clamp(puddles, 0.0, 1.0) * level,
                 0.0, 1.0);
}

float wetnessMask(vec3 N, float ao, float amount, float puddles) {
    // EXPOSURE. N.y = 1 is a puddle-flat surface, 0 is a wall, -1 is a soffit.
    // The smoothstep keeps the wall-to-underside falloff soft; a hard step
    // draws a visible waterline across curved geometry.
    float up      = clamp(N.y, -1.0, 1.0);
    float exposed = mix(kWetVerticalFraction, 1.0, smoothstep(0.0, 0.55, up));
    // Undersides: fade to bone dry as the face turns past vertical. This is
    // what keeps bridge soffits and tunnel roofs dry.
    exposed *= smoothstep(-0.35, 0.0, up);

    // POOLING. Occluded geometry dishes inward and holds water. ao == 1 (open
    // ground) gets the base coverage; a deep cavity gets up to ~1.55x, clamped
    // below so `puddles = 0` is exactly "no pooling term", not "less wet".
    float cavity  = (1.0 - clamp(ao, 0.0, 1.0)) * puddles;
    float pooling = 1.0 + cavity * 0.55;

    return clamp(amount * exposed * pooling, 0.0, 1.0);
}

// Apply wetness in place to the material terms mesh.frag is about to shade with.
//   albedo   : base colour, darkened by the film
//   pRough   : perceptual roughness, pulled toward the film roughness
//   F0       : dielectric reflectance, nudged toward the water interface
//   N, ao    : for the coverage mask
//   metallic : metals are excluded (see the header note)
//   w        : ssao.wetness — x = amount, y = porosity, z = puddles, w = minRough
// Returns the coverage it used, so callers can debug-visualise it.
// `gloss` is the surface's PERVIOUSNESS, inverted: 1 = impervious (asphalt,
// concrete, stone, painted steel — water sits ON it as a film and it turns
// specular), 0 = pervious (soil, grass, scree, bare terrain — water soaks IN,
// so it darkens and never glosses). This distinction is not decoration: the
// first A/B lit the cliff faces with blown specular streaks because the model
// was treating a wet hillside like wet tarmac. Rock and grass in the rain get
// DARKER, not shinier. Darkening applies to both; only glossing is gated.
float applyWetness(inout vec3 albedo, inout float pRough, inout vec3 F0,
                   vec3 N, float ao, float metallic, vec4 w, float gloss) {
    // THE GATE. Dry worlds leave here having done one compare.
    if (w.x <= 0.0) return 0.0;

    float wet = wetnessMask(N, ao, w.x, w.z);
    if (wet <= 0.0) return 0.0;

    // A conductor's own F0/diffuse must not be lerped by a surface film, so
    // fade the whole effect out as metallic rises.
    wet *= (1.0 - clamp(metallic, 0.0, 1.0));
    if (wet <= 0.0) return 0.0;

    const float filmRough = w.w > 0.0 ? w.w : kWetFilmRoughFallback;

    // POROSITY FROM ROUGHNESS (see header note 1). A rough surface has the open
    // micro-structure that traps light once it is wet; a smooth one does not.
    // Computed from the DRY roughness, before the film smooths it — the water
    // has not changed what the substrate is made of.
    float porous = clamp(pRough, 0.0, 1.0) * clamp(w.y, 0.0, 1.0);

    // 1. Diffuse darkening.
    albedo *= mix(1.0, mix(1.0, kWetMaxDarken, porous), wet);

    // 2. Smoothing — and NOT uniformly to the film roughness, which was the
    // first version's mistake. A thin film FOLLOWS the substrate's micro-relief:
    // wet gravel, wet rock and wet grass are darker and glossier, but they are
    // not mirrors. Only water that has POOLED levels out into a true specular
    // surface. Collapsing every wet fragment to filmRough put a near-mirror
    // lobe on the cliff faces and lit them with blown-out specular streaks
    // under the sun — visible immediately in the first A/B capture.
    //
    // So the target is the substrate, PARTLY levelled, and only the pooled
    // fraction goes all the way down to the film.
    // Glossing — and ONLY glossing — is scaled by how impervious the surface is.
    float g = clamp(gloss, 0.0, 1.0);
    if (g > 0.0) {
        float pooled   = wetnessPooling(N, ao, w.z);
        float levelled = mix(pRough * kWetCoatRoughScale, filmRough, pooled);
        pRough = mix(pRough, min(pRough, levelled), wet * g);

        // 3. Dielectric F0 toward the air-water interface. max(), not mix(), so
        // a surface already glossier than water is never dulled by rain.
        F0 = max(F0, vec3(mix(0.04, 0.05, wet * g)));
    }

    return wet;
}
