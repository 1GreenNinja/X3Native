#ifndef X3_MESH_IBL_GLSL
#define X3_MESH_IBL_GLSL
// Split-sum IBL ambient (Karis/Epic). Returns the combined diffuse + specular
// environment contribution in LINEAR HDR. `perceptualRough` is glTF roughness
// (NOT alpha). `ao` modulates both lobes (specular gets a milder occlusion).
// Falls back to the previous flat ambient*Fresnel constant when no env is baked.
// `ddgi`: rgb = probe-field irradiance (already intensity-scaled), a = grid
// confidence — REPLACES the ambient DIFFUSE term by confidence when active
// (specular stays IBL/reflections); a == 0 leaves the math byte-identical.
// ---- THE ENGINE HAS TWO AMBIENTS AND ONLY ONE OF THEM IS DOCUMENTED ---------------
// (2026-07-12, fix/prim-point-light.) Below, on the BAKED-ENVIRONMENT path, the
// `ambient` argument — i.e. everything `setAmbient()` controls, the dial the whole
// "AMBIENT IS NOT LIGHT, BRING IT DOWN" doctrine turns — is NEVER READ for the
// diffuse or the specular. It only survives as the metal floor. An environment is
// baked by DEFAULT for every scene (from the ANALYTIC SKY unless a host calls
// setIblProbe), so in practice `setAmbient` has been a NO-OP in most of the game and
// the true ambient has been a full-strength blue sky cube that nobody could see in
// the code. That is how a windowless basement ended up lit blue.
// THE DIALS ARE NOW COHERENT: iblIntensity == 0 means "this room has no environment"
// and falls through to the flat-ambient path, where setAmbient does exactly what it
// says. Every existing host (intensity 0.22 / 0.5 / 1.0) is byte-identical.
vec3 iblAmbient(vec3 N, vec3 V, vec3 albedo, float metallic, float perceptualRough,
                vec3 F0, float ao, vec3 ambient, float up, vec4 ddgi) {
    if (ssao.ibl.x < 0.5 || ssao.ibl.y <= 0.0) {
        // FALLBACK (no baked environment): the original engine behaviour exactly —
        // diffuse hemispheric lift + the flat ambient*3.4*Fresnel specular constant.
        // DDGI (when active) replaces the flat DIFFUSE irradiance by confidence.
        float NoV = max(dot(N, V), 1e-4);
        float a   = perceptualRough; a *= a;
        vec3  diff = albedo * (1.0 - metallic);
        vec3  diffuseIrr = mix(ambient * mix(0.85, 1.25, up), ddgi.rgb, ddgi.a);
        vec3  amb  = diffuseIrr * ao * diff;
        vec3  Fr   = F0 + (max(vec3(1.0 - a), F0) - F0) * pow(1.0 - NoV, 5.0);
        amb += (ambient * 3.4) * Fr * mix(0.55, 1.1, up) * ao;
        return amb;
    }
    float NoV = max(dot(N, V), 1e-4);
    float maxMip = max(ssao.ibl.z, 0.0);
    float intensity = ssao.ibl.y;

    // Roughness-aware Fresnel for the energy split between diffuse + specular IBL.
    vec3 F  = F_SchlickRoughness(NoV, F0, perceptualRough);
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    // Diffuse IBL: irradiance(N) already carries the PI-weighted hemisphere integral.
    // DDGI (r_ddgi): the traced probe field REPLACES the env-cube irradiance by
    // grid confidence — same E units, so the kD/albedo/ao weighting below applies
    // exactly once either way. Outside the grid (a -> 0) the env cube remains.
    vec3 irradiance = texture(irradianceCube, N).rgb;
    irradiance = mix(irradiance, ddgi.rgb, ddgi.a);
    vec3 diffuse = irradiance * albedo;

    // Specular IBL: prefiltered radiance along the reflection vector at mip=rough,
    // scaled by the split-sum env BRDF (F0*scale + bias).
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(prefilterCube, R, perceptualRough * maxMip).rgb;

    // ---- SSR / RT reflections (r_ssr, carried in ssao.refl.x) --------------
    // Where the reflection pass produced a confident hit, its radiance REPLACES
    // the prefiltered env radiance — same units (linear HDR incoming radiance
    // along R), so it rides the IDENTICAL split-sum weighting below (F0*scale +
    // bias), the metal ambient floor and the Reinhard rolloff. Energy-conserving
    // by construction: a blend, never an addition on top of full IBL specular.
    // Roughness gate: the traced ray is MIRROR-sharp, so it only stands in for
    // the env lobe on polished surfaces — full strength below rough 0.25, faded
    // out by rough 0.6 where the prefiltered (properly blurred) env takes over.
    if (ssao.refl.x > 0.5) {
        vec2 ruv = gl_FragCoord.xy * ssao.ctrl.zw;       // pixel -> [0,1] screen UV
        // Roughness-aware: blur the traced reflection instead of discarding it,
        // so glossy-but-not-mirror surfaces keep real reflected detail. The
        // hand-off to the prefiltered env now happens much later (0.55 -> 0.95)
        // because the reflection is no longer wrong at mid roughness -- it is
        // simply softer. Mirror surfaces are unchanged (single tap, same weight).
        vec4 rr  = sampleReflGlossy(ruv, perceptualRough, ssao.ctrl.zw);
        float rw = clamp(rr.a, 0.0, 1.0) * clamp(ssao.refl.y, 0.0, 1.0)
                 * (1.0 - smoothstep(0.55, 0.95, perceptualRough));
        prefiltered = mix(prefiltered, rr.rgb, rw);
    }

    vec2 ab = texture(brdfLUT, vec2(NoV, perceptualRough)).rg;
    vec3 specular = prefiltered * (F0 * ab.x + ab.y);

    // (the env-specular scale is applied at the return, alongside intensity)

    // ---- Metal ambient-specular FLOOR (r_metalambient, carried in ssao.ibl.w) ----
    // Metals have no diffuse lobe (kD ~ 0 above), so when the baked environment is
    // DARK (night interiors, windowless rooms) their entire ambient response is this
    // prefiltered specular — which is then ~0, and metals render near-black even
    // though the scene has a healthy flat ambient (cam.ambientCount). Physically a
    // metal in a lit room still shows an F0-tinted environment response (HDRP gives
    // metals exactly this ambient specular floor). Floor the env specular at the
    // scene's hemispheric ambient tinted by F0, gated by metallic so DIELECTRICS
    // (F0 = 0.04) are untouched, and dimmed for rough metals (duller response).
    // max(), not +=: a healthy/bright environment is NEVER brightened, and the floor
    // passes through the same Reinhard energy rolloff below as the env specular.
    vec3 floorSpec = ssao.ibl.w * ambient * F0 * metallic
                   * mix(1.0, 0.55, perceptualRough) * mix(0.55, 1.1, up);
    specular = max(specular, floorSpec);

    // Energy ceiling: a near-mirror metal (low roughness, high F0) reflecting a bright
    // environment produces HDR specular so large it clips past ACES to flat white. Soft
    // per-channel Reinhard rolloff: bright reflections compress gracefully toward 1 while
    // dim ones pass through nearly unchanged (x/(1+x): 0.1->0.09, 1->0.5, 4->0.8).
    specular = specular / (1.0 + specular);

    // Specular occlusion: a softer AO on the specular lobe so recesses still darken
    // reflections (full AO would kill them). Diffuse takes the full AO.
    float specAo = clamp(ao + 0.4, 0.0, 1.0);

    // ---- SPLIT SCALES: env DIFFUSE and env SPECULAR are different lobes ---------
    // `intensity` (ibl.y) used to scale BOTH, which makes "dark moody interior" and
    // "bright reflective metal" mutually exclusive: raise the environment enough for
    // steel to reflect it and you flood every dielectric in the room with irradiance;
    // lower it to protect the mood and metals have nothing left to reflect. But the
    // two lobes have different owners -- a METAL is kD ~ 0, so the prefiltered env
    // specular IS its entire ambient response, while CONCRETE is kD ~ 1 and almost
    // pure diffuse. One scale cannot serve both, and the rifthub proved it: the gate
    // was a mirror aimed at a black room.
    // refl.z (r_iblspec) is the ABSOLUTE env-specular scale. <= 0 means "unset" and
    // falls back to `intensity`, so every world that never calls setIblSpecular() is
    // byte-for-byte the pre-R10 math.
    //
    // GATED ON METALLIC, and that is the whole point. A dielectric (concrete, plaster,
    // the hall's WET FLOOR at metal 0.09) keeps `intensity` exactly as calibrated --
    // so turning the environment up for the steel does NOT wash the room, which is the
    // failure the rifthub has hit in rounds 2, 5 and 9. A metal (the gate at 0.65) has
    // no diffuse lobe at all, so this reflection IS its light, and it gets the dome.
    // Same environment, two materials, two honest responses.
    float specScale = (ssao.refl.z > 0.0) ? ssao.refl.z : intensity;
    float sScale    = mix(intensity, specScale, metallic);
    return kD * diffuse * ao * intensity + specular * specAo * sScale;
}
#endif  // X3_MESH_IBL_GLSL
