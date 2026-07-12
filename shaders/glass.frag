#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Translucent GLASS fragment shader — the transparent pass companion to mesh.frag
// (design spec docs/superpowers/specs/2026-05-25-glass-material-design.md).
//
// Drawn in a dedicated post-opaque pass: depth-tested LESS_OR_EQUAL against the
// opaque depth, depth-write OFF, alpha-blended (SRC_ALPHA / ONE_MINUS_SRC_ALPHA)
// into the SAME linear HDR scene the opaque pass produced. It shares the mesh
// pipeline's vertex shader (mesh.vert), the four mesh descriptor sets (bindless
// textures set0, camera UBO+SSBO set1, shadow map set2, SSAO set3) AND a glass-only
// set4 (scene-color copy + GlassControl UBO), so it reads the exact same per-object
// payload PLUS the screen behind it.
//
// MILESTONES:
//   M1 (alpha see-through): lit like the opaque mesh, output alpha = opacity.
//   M2 (screen-space REFRACTION): sample a COPY of the scene captured before this
//      pass at screenUV + (screen-space normal * refraction), so the scene behind
//      the glass BENDS. Strength from GlassMaterial.refraction.
//   M3 (fresnel + specular shimmer): sun/point GGX glints + environment sheen.
//   M3-R (W8-2): REAL environment reflection — the split-sum IBL cubes the opaque
//      path uses (set 5 = the same m_iblMeshSet: prefiltered env + BRDF LUT), so
//      glass MIRRORS the sky/environment with proper fresnel. This REPLACED the
//      old world-position sine "glint band", which painted parameter-immune
//      DIAGONAL STREAKS across large panes (the W3-3 tower artifact: a ~2.6 m
//      period stripe field over any facade-sized surface).
//   M4 (roughness / frost): frosted transmission via pre-blurred scene copy.
// NON-glass fragments DISCARD here (they belong to the opaque pass).

layout(set = 0, binding = 0) uniform sampler2D textures[];

struct PointLight {
    vec4 posRange;   // xyz = world position, w = range (meters)
    vec4 colorPad;   // rgb = linear color * intensity, a = unused
};
const int kMaxPointLights = 64;

layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 ambientCount;              // rgb = ambient color, w = active light count
    PointLight lights[kMaxPointLights];
    vec4 camPos;                    // xyz = camera world position (unused here; g.camPos wins)
    vec4 sunDir;                    // xyz = per-scene direction TOWARD the sun (FrameUBO tail)
} cam;

layout(set = 2, binding = 0) uniform sampler2DShadow shadowMap;

layout(set = 3, binding = 0) uniform sampler2D ssaoTex;
// Same UBO the opaque path reads (m_meshAoSet binding 1) — glass declares the
// PREFIX it needs: ctrl + the IBL lane (x=valid, y=intensity, z=prefilter max
// mip). Offsets match mesh.frag's fuller declaration by construction.
layout(set = 3, binding = 1) uniform SsaoControl {
    vec4 ctrl;
    vec4 ibl;         // x=IBL valid(0/1), y=IBL intensity, z=prefilter max mip
} ssao;

// ---- Glass-only set 4 -----------------------------------------------------
// binding 0: the scene-color COPY captured AFTER the opaque pass, BEFORE this
//            pass (so we can sample the scene behind the glass while writing HDR).
//            Mip-chained for the frost lookup (M4); M2 samples mip 0.
// binding 1: per-frame control (camera pos + time + screen->UV + dev overrides +
//            camera right/up for the screen-space normal projection).
layout(set = 4, binding = 0) uniform sampler2D sceneCopy;     // sharp scene behind glass
layout(set = 4, binding = 1) uniform GlassControl {
    vec4 camPos;     // xyz = camera world pos, w = time
    vec4 screen;     // x = 1/W, y = 1/H, z = frostReady (0/1), w = sceneCopyValid (0/1)
    vec4 ctrl;       // x = refractScale, y = roughAdd, z = specScale, w = overrideOn
    vec4 camRight;   // xyz = camera RIGHT axis (world)
    vec4 camUp;      // xyz = camera UP axis (world)
} g;
layout(set = 4, binding = 2) uniform sampler2D sceneFrost;   // blurred scene (M4 frost)

// ---- IBL set 5 (M3-R) — the SAME descriptor set the opaque mesh path binds at
// set 4 (m_iblMeshSet): binding 1 = GGX-prefiltered env radiance (roughness in
// the mip chain), binding 2 = split-sum BRDF LUT. Binding 0 (irradiance) exists
// in the set but glass doesn't sample it (reflection only, no diffuse ambient).
// Gated by ssao.ibl.x, exactly like mesh.frag.
layout(set = 5, binding = 1) uniform samplerCube prefilterCube;
layout(set = 5, binding = 2) uniform sampler2D brdfLUT;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 3) flat in vec4 vFactor;       // rgb tint*texel, a = OPACITY (glass)
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) flat in vec4 vEmissive;     // rgb = color, a = strength
layout(location = 6) flat in uint vFlags;        // bit0 = TERRAIN, bit1 = GLASS
layout(location = 7) flat in uvec2 vTerrainPack; // unused for glass
// Locations 8-11 carry the PBR map indices (mesh.frag consumes them; glass ignores).
// Glass material rides at 12/13 to match the merged mesh.vert output interface.
layout(location = 8)  flat in uint vNormalTexIndex;   // (unused by glass)
layout(location = 9)  flat in uint vMrTexIndex;       // (unused by glass)
layout(location = 10) flat in uint vEmissiveTexIndex; // (unused by glass)
layout(location = 11) flat in uint vDetailPacked;     // (unused by glass)
layout(location = 12) flat in vec4 vGlassParams;  // x = refraction, y = roughness, z = specular
layout(location = 13) flat in vec4 vGlassTint;    // rgb = tint, a = emissiveMap (0 flat .. 1 texel-modulated)

layout(location = 0) out vec4 outColor;

const uint FLAG_GLASS = 2u;

// Sun direction is PER-SCENE (cam.sunDir, filled from SkyParams — defaults to
// the old hardcoded (0.4,1,0.3) when no sky is set, so indoor worlds are
// unchanged). W8-2: the old const here meant a golden-hour host's LOW sun never
// matched the glass specular — the glint sat where no sun was.
const vec3 kSunColor = vec3(1.0, 0.97, 0.92);

// 3x3 PCF (identical to mesh.frag) — glass is still lit by the sun so it doesn't
// read as a flat slab in the dark.
float sampleShadow(vec3 worldPos, float ndl) {
    vec4 lc = cam.lightViewProj * vec4(worldPos, 1.0);
    vec3 proj = lc.xyz / lc.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    float curDepth = proj.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || curDepth > 1.0)
        return 1.0;
    float bias = clamp(0.0015 * tan(acos(clamp(ndl, 0.0, 1.0))), 0.0005, 0.004);
    float refDepth = curDepth - bias;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += texture(shadowMap, vec3(uv + vec2(x, y) * texel, refDepth));
    return lit / 9.0;
}

float pointAtten(float dist, float range) {
    float t = dist / max(range, 0.0001);
    float w = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    w *= w;
    return w / (dist * dist + 1.0);
}

// ---- M3: fresnel + specular shimmer helpers --------------------------------
// Schlick fresnel: reflectance rises toward grazing angles. F0 ~ 0.04 (glass/
// dielectric at normal incidence). cosTheta = dot(N, V). Clean-room standard.
float fresnelSchlick(float cosTheta, float F0) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return F0 + (1.0 - F0) * (m2 * m2 * m);   // F0 + (1-F0)(1-cos)^5
}

// GGX (Trowbridge-Reitz) normal distribution for the specular highlight lobe.
// rough in (0,1]; smaller rough -> sharper, brighter glint. The standard form.
float ggxSpec(vec3 N, vec3 H, float rough) {
    float a  = max(rough * rough, 1e-3);
    float a2 = a * a;
    float ndh = max(dot(N, H), 0.0);
    float d = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-4);
}

// A single light's specular contribution (Blinn-Phong half-vector + GGX lobe).
vec3 specLight(vec3 N, vec3 V, vec3 L, vec3 radiance, float rough) {
    vec3 H = normalize(L + V);
    float spec = ggxSpec(N, H, rough);
    // Normalize the lobe energy a touch so low roughness doesn't blow out.
    return radiance * spec * 0.25;
}

void main() {
    // Only GLASS-flagged fragments belong to this pass; everything else is opaque
    // and already rendered.
    if ((vFlags & FLAG_GLASS) == 0u) discard;

    vec3 N = normalize(vNormal);

    // ---- Per-object glass material (M2-M4), with optional live dev override. ----
    // ctrl.w == 1 -> scale/add the authored material (r_glass_* scrubbing).
    float refraction = vGlassParams.x;
    float roughness  = clamp(vGlassParams.y, 0.0, 1.0);
    float specular   = vGlassParams.z;
    if (g.ctrl.w > 0.5) {
        refraction *= g.ctrl.x;
        roughness   = clamp(roughness + g.ctrl.y, 0.0, 1.0);
        specular   *= g.ctrl.z;
    }
    // View vector (fragment -> camera) for fresnel + specular.
    vec3 V = normalize(g.camPos.xyz - vWorldPos);
    float time = g.camPos.w;

    // Body color: the bound texture (holo UI / white) tinted by the glass color.
    // vFactor.rgb carries the per-object baseColor; vGlassTint.rgb is the glass
    // tint. Treat white tint (default) as colorless.
    vec3 texel = texture(textures[nonuniformEXT(vTexIndex)], vUV).rgb;
    vec3 tint  = vGlassTint.rgb;
    vec3 body  = texel * vFactor.rgb * tint;

    // ---- STREET LIGHT: ADDITIVE GLOW mode (vGlassParams.w > 0) -------------
    // Fake volumetric light surfaces (street-lamp cones / ground light pools).
    // The glow = per-object emissive * the bound gradient texture (the axial /
    // radial falloff bake) * a view-angle rim fade pow(dot(N,V), w) so the
    // silhouette edges of a cone melt away instead of reading as a hard-edged
    // cylinder; w is authored per material (~1.5 cones, ~0.05 flat pools).
    // Back faces have dot(N,V) <= 0 and contribute nothing. Output uses a tiny
    // constant alpha with the energy pre-divided out, so under this pass's
    // SRC_ALPHA/ONE_MINUS_SRC_ALPHA blend the result is glow + 0.965*dst:
    // effectively additive, and OVERLAPPING glows accumulate (the haveScene
    // "replace" path would erase the cone behind this one). No refraction, no
    // specular, no shadow taps — the cheapest fragment in the pass.
    if (vGlassParams.w > 0.0) {
        vec3 Vv = normalize(g.camPos.xyz - vWorldPos);
        float rim = pow(max(dot(N, Vv), 0.0), vGlassParams.w);
        vec3 glow = vEmissive.rgb * vEmissive.a * texel * tint * rim;
        const float kAddA = 0.035;
        outColor = vec4(glow / kAddA, kAddA);
        return;
    }

    // ---- M2: screen-space REFRACTION + M4: ROUGHNESS / FROST -------------
    // Project the world-space surface normal onto the screen plane (camera right/up)
    // to get the in-screen bend direction, then sample the scene-color copy at the
    // refraction-offset UV. The copy is the scene as it looked BEFORE this glass was
    // drawn (you can't sample + write the live HDR target in one pass). M4: also
    // sample a pre-blurred copy and LERP sharp->frosted by roughness, so the scene
    // through the glass goes from crisp (polished) to milky (frosted).
    vec2 screenUV = gl_FragCoord.xy * g.screen.xy;
    vec3 refractedScene = vec3(0.0);
    bool haveScene = g.screen.w > 0.5;
    bool haveFrost = g.screen.z > 0.5;
    if (haveScene) {
        vec2 nScreen = vec2(dot(N, g.camRight.xyz), dot(N, g.camUp.xyz));
        vec2 offUV   = nScreen * refraction;
        vec2 sampUV  = clamp(screenUV + offUV, vec2(0.0), vec2(1.0));
        vec3 sharp   = textureLod(sceneCopy, sampUV, 0.0).rgb;
        if (haveFrost && roughness > 0.001) {
            // Frosted glass scatters: a frosted pane jitters the lookup a touch (so
            // even the blurred sample isn't a clean mirror of the scene) and leans on
            // the blurred copy. smoothstep gives a gentle ramp so low roughness stays
            // nearly clear. Sample the blurred copy at the SAME offset UV.
            vec3 frost = textureLod(sceneFrost, sampUV, 0.0).rgb;
            float fmix = smoothstep(0.0, 0.85, roughness);
            refractedScene = mix(sharp, frost, fmix);
        } else {
            refractedScene = sharp;
        }
    }

    // ---- Lighting (same model as the opaque mesh) — used for the glass BODY ----
    // The specular accumulator (M3) collects the bright glints the BODY lighting
    // would miss: a sharp lobe off the sun + the nearby point lights, scaled by the
    // material's specular and sharpened by (1 - roughness).
    vec3 kSunDir = normalize(cam.sunDir.xyz);      // per-scene sun (Camera UBO tail)
    float specRough = mix(0.06, 0.6, roughness);   // polished -> sharp, frosted -> broad
    float ndl = max(dot(N, kSunDir), 0.0);
    float shadow = sampleShadow(vWorldPos, ndl);
    vec3 lighting = kSunColor * (0.75 * ndl * shadow);
    vec3 specSum = specLight(N, V, kSunDir, kSunColor, specRough) * shadow;

    vec3 ambient = cam.ambientCount.rgb;
    float up = N.y * 0.5 + 0.5;
    float ao = 1.0;
    if (ssao.ctrl.x > 0.5) {
        vec2 aoUV = gl_FragCoord.xy * ssao.ctrl.zw;
        float aoSample = texture(ssaoTex, aoUV).r;
        ao = mix(1.0, aoSample, clamp(ssao.ctrl.y, 0.0, 1.0));
    }
    lighting += ambient * mix(0.85, 1.25, up) * ao;

    int n = int(cam.ambientCount.w);
    for (int i = 0; i < n && i < kMaxPointLights; ++i) {
        vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
        float dist = length(toL);
        vec3  L    = toL / max(dist, 0.0001);
        float pndl = max(dot(N, L), 0.0);
        float att  = pointAtten(dist, cam.lights[i].posRange.w);
        lighting  += cam.lights[i].colorPad.rgb * (pndl * att);
        specSum   += specLight(N, V, L, cam.lights[i].colorPad.rgb, specRough) * att;
    }

    // ---- M3-R: Schlick FRESNEL + REAL environment reflection --------------
    // Fresnel: glass reflects more at grazing angles + edges (cosTheta -> 0). This
    // both BRIGHTENS the rim and LIFTS the alpha so a crystal-clear pane still
    // reads as a surface instead of an invisible plane.
    //
    // W8-2: the old "animated glint" here — smoothstep(sin(dot(vWorldPos, k)))
    // — was a WORLD-SPACE STRIPE GENERATOR: on facade-sized panes it painted
    // parallel diagonal streaks (~2.6 m period) that survived every material
    // parameter change (the W3-3 tower artifact; only `specular` scaled it).
    // REMOVED, replaced by the split-sum environment reflection the opaque path
    // uses: prefiltered env radiance along R at the roughness mip, weighted by
    // the BRDF LUT (which carries the fresnel rise toward grazing), with the
    // same Reinhard energy rolloff mesh.frag applies so a mirror of the sun
    // compresses gracefully instead of clipping to flat white.
    float cosV    = max(dot(N, V), 0.0);
    float fresnel = fresnelSchlick(cosV, 0.04);     // dielectric F0
    vec3 envRefl;
    if (ssao.ibl.x > 0.5) {
        vec3 R = reflect(-V, N);
        vec3 prefiltered = textureLod(prefilterCube, R, roughness * max(ssao.ibl.z, 0.0)).rgb;
        vec2 ab = texture(brdfLUT, vec2(max(cosV, 1e-4), clamp(roughness, 0.0, 1.0))).rg;
        vec3 spec = prefiltered * (vec3(0.04) * ab.x + ab.y) * ssao.ibl.y;
        envRefl = spec / (1.0 + spec);              // Reinhard rolloff (mesh.frag parity)
    } else {
        // No baked environment: the old ambient-tinted fresnel sheen (minus the
        // stripe band) so glass never reads as an invisible plane.
        vec3 sheen = mix(kSunColor, ambient + vec3(0.04), 0.5);
        envRefl = sheen * (fresnel * 0.6);
    }

    // The full specular term: GGX sun/point glints + the environment reflection,
    // all scaled by the material's specular.
    vec3 specOut = (specSum + envRefl) * max(specular, 0.0);

    // ---- Compose the glass colour ----------------------------------------
    // Split the body into its DIFFUSE part (the lit tinted surface — gated by opacity
    // against what is seen through the glass) and ADDITIVE light: emissive glow +
    // specular/fresnel shimmer. The additive light always rides ON TOP (a hologram's
    // glow / a glint isn't hidden by a low opacity), which keeps the holo-terminal's
    // emissive sweep + glints bright while still letting the glass read as see-through.
    vec3 litDiffuse = body * lighting;
    // EMISSIVE MAP (GlassMaterial::emissiveMap -> vGlassTint.a). At 0 (every legacy
    // pane) the glow is FLAT across the surface — byte-identical to before. At 1 it is
    // modulated by the bound base-color TEXEL, so a DISPLAY glass (the holo terminal)
    // glows exactly where its readout is bright and its BLACK substrate stays BLACK.
    // Without this the only way to make a glass screen glow is to flood the whole pane
    // with a flat colour — which is precisely how the holo screens became featureless
    // blue slabs. (Glass-pass twin of the opaque PBR route's emissiveTex.)
    vec3 emisMask   = mix(vec3(1.0), texel, clamp(vGlassTint.a, 0.0, 1.0));
    vec3 additive   = vEmissive.rgb * vEmissive.a * emisMask + specOut;   // glow + shimmer (feeds bloom)

    float opacity = clamp(vFactor.a, 0.0, 1.0);

    if (haveScene) {
        // SCREEN-SPACE refraction path: we have a copy of the scene behind the glass,
        // so this fragment SUPPLIES its own background (the BENT scene) rather than
        // letting the alpha blend reveal the un-bent live scene. Mix the (refracted,
        // tinted) scene with the lit diffuse by opacity (clear pane -> mostly bent
        // scene; opaque -> mostly body), then ADD the glow + shimmer. Output alpha = 1
        // so the composed result fully replaces the live HDR pixel (which holds the
        // SAME, but un-bent, scene).
        vec3 throughGlass = refractedScene * mix(vec3(1.0), tint, 0.5);
        outColor = vec4(mix(throughGlass, litDiffuse, opacity) + additive, 1.0);
    } else {
        // No scene copy (target failed / disabled): fall back to the M1 alpha
        // see-through path (SRC_ALPHA blend reveals the live scene). Premultiply the
        // additive glow/shimmer by (1/alpha won't work) — instead add it post-blend by
        // baking it into the colour and LIFTING the alpha by fresnel so a low-opacity
        // pane still catches light at its edges (never an invisible plane).
        float outA = clamp(opacity + fresnel * (1.0 - opacity), 0.0, 1.0);
        // Scale the additive term up by 1/outA so the SRC_ALPHA blend (which multiplies
        // colour by outA) preserves the intended glow energy.
        vec3 color = litDiffuse + additive / max(outA, 0.04);
        outColor = vec4(color, outA);
    }
}
