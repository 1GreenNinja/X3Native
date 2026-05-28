#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Translucent GLASS fragment shader — the transparent pass companion to mesh.frag
// (design spec docs/GLASS_MATERIAL_SPEC.md). M5 rework: a genuinely SHINY +
// TRANSPARENT UE5-modeled material (Filament Cook-Torrance BRDF + roughness-aware
// fresnel environment reflection), clean-room from the public Filament / LearnOpenGL
// / UE5 docs.
//
// Drawn in a dedicated post-opaque pass: depth-tested LESS_OR_EQUAL against the
// opaque depth, depth-write OFF, alpha-blended (SRC_ALPHA / ONE_MINUS_SRC_ALPHA)
// into the SAME linear HDR scene the opaque pass produced. It shares the mesh
// pipeline's vertex shader (mesh.vert), the four mesh descriptor sets (bindless
// textures set0, camera UBO+SSBO set1, shadow map set2, SSAO set3) AND a glass-only
// set4 (scene-color copy + GlassControl UBO + frost-blur), so it reads the exact
// same per-object payload PLUS the screen behind it.
//
// COMPOSITE (energy-split, see GLASS_MATERIAL_SPEC.md):
//   throughGlass = refracted scene behind (roughness -> blur via the frost copy),
//                  lightly tinted by transmittanceColor.
//   reflection   = fresnel-weighted environment (screen reflection + sky gradient).
//   specSum      = analytic Cook-Torrance glints off the sun + point lights.
//   interior     = mix(throughGlass, litBody, opacity)
//   color        = interior*kT + reflection + specSum + emissive
//   With a scene copy we write vec4(color,1) (opaque-replace: we supply the bent
//   background); else alpha-blend lifted by fresnel.
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
} cam;

layout(set = 2, binding = 0) uniform sampler2DShadow shadowMap;

layout(set = 3, binding = 0) uniform sampler2D ssaoTex;
layout(set = 3, binding = 1) uniform SsaoControl {
    vec4 ctrl;
} ssao;

// ---- Glass-only set 4 -----------------------------------------------------
// binding 0: the scene-color COPY captured AFTER the opaque pass, BEFORE this
//            pass (so we can sample the scene behind the glass while writing HDR).
// binding 1: per-frame control (camera pos + time + screen->UV + dev overrides +
//            camera right/up for the screen-space normal/reflection projection).
// binding 2: a pre-blurred copy of the scene (frost) — leaned on by roughness.
layout(set = 4, binding = 0) uniform sampler2D sceneCopy;     // sharp scene behind glass
layout(set = 4, binding = 1) uniform GlassControl {
    vec4 camPos;     // xyz = camera world pos, w = time
    vec4 screen;     // x = 1/W, y = 1/H, z = frostReady (0/1), w = sceneCopyValid (0/1)
    vec4 ctrl;       // x = refractScale, y = roughAdd, z = specScale, w = overrideOn
    vec4 camRight;   // xyz = camera RIGHT axis (world)
    vec4 camUp;      // xyz = camera UP axis (world)
} g;
layout(set = 4, binding = 2) uniform sampler2D sceneFrost;   // blurred scene (frost)

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 3) flat in vec4 vFactor;       // rgb tint*texel, a = OPACITY (glass)
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) flat in vec4 vEmissive;     // rgb = color, a = strength
layout(location = 6) flat in uint vFlags;        // bit0 = TERRAIN, bit1 = GLASS
layout(location = 7) flat in uvec2 vTerrainPack; // unused for glass
layout(location = 8) flat in vec4 vGlassParams;  // x = refraction, y = roughness, z = specular, w = metallic
layout(location = 9) flat in vec4 vGlassTint;    // rgb = tint (baseColor), w = ior
layout(location = 10) flat in vec4 vGlassExtra;  // x = reflectance, yzw = transmittanceColor

layout(location = 0) out vec4 outColor;

const uint FLAG_GLASS = 2u;

const vec3 kSunDir   = normalize(vec3(0.4, 1.0, 0.3));
const vec3 kSunColor = vec3(1.0, 0.97, 0.92);

// ---- BRDF + fresnel constants (Filament / split-sum IBL; clean-room) -------
const float PI = 3.14159265;
const float MIN_PERCEPTUAL_ROUGH = 0.089;
const float MAX_REFLECTION_LOD   = 5.0;   // sceneCopy mip count - 1 (LOD clamps if fewer)

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

// ---- Filament Cook-Torrance BRDF + UE5 F0 (clean-room, public docs) --------
// F0 from IOR (dielectric) or baseColor (metal). For glass ior 1.5 -> 0.04.
// When ior <= 1 (authoring chose the UE5 "Specular"/reflectance dial instead),
// fall back to 0.16*reflectance^2 (Filament's remapping).
vec3 computeF0(vec3 baseColor, float metallic, float ior, float reflectance) {
    float f0d;
    if (ior > 1.0) { float t = (ior - 1.0) / (ior + 1.0); f0d = t * t; }
    else           { f0d = 0.16 * reflectance * reflectance; }
    return mix(vec3(f0d), baseColor, metallic);
}
// GGX (Trowbridge-Reitz) normal distribution; a = roughness^2 (linear roughness).
float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float d  = (NoH * NoH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}
// Height-correlated Smith visibility (includes the 1/(4 NoL NoV) denominator).
float V_SmithGGX(float NoV, float NoL, float a) {
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
// Schlick fresnel (analytic lights).
vec3 F_Schlick(float u, vec3 f0) {
    float f = pow(1.0 - u, 5.0);
    return f0 + (1.0 - f0) * f;
}
// Roughness-aware fresnel for the environment term: a rough surface keeps a dimmer,
// broader rim (Sebastien Lagarde). pr = perceptual roughness.
vec3 F_SchlickRough(float NoV, vec3 f0, float pr) {
    return f0 + (max(vec3(1.0 - pr), f0) - f0) * pow(1.0 - NoV, 5.0);
}
// One analytic light's Cook-Torrance specular contribution (already * NoL).
vec3 specBRDF(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 f0, float a) {
    vec3 H = normalize(L + V);
    float NoV = max(dot(N, V), 1e-4);
    float NoL = max(dot(N, L), 0.0);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);
    return (D_GGX(NoH, a) * V_SmithGGX(NoV, NoL, a)) * F_Schlick(VoH, f0) * radiance * NoL;
}

void main() {
    // Only GLASS-flagged fragments belong to this pass; everything else is opaque
    // and already rendered.
    if ((vFlags & FLAG_GLASS) == 0u) discard;

    // ---- Per-object glass material, with optional live dev override. ----
    // ctrl.w == 1 -> scale/add the authored material (r_glass_* scrubbing).
    float refraction = vGlassParams.x;
    float roughness  = clamp(vGlassParams.y, 0.0, 1.0);
    float specular   = vGlassParams.z;
    float metallic   = clamp(vGlassParams.w, 0.0, 1.0);
    float ior        = vGlassTint.w;
    float reflectance = vGlassExtra.x;
    vec3  transmit   = vGlassExtra.yzw;
    if (g.ctrl.w > 0.5) {
        refraction *= g.ctrl.x;
        roughness   = clamp(roughness + g.ctrl.y, 0.0, 1.0);
        specular   *= g.ctrl.z;
    }
    // Defaults if a draw left a lane zeroed (legacy glass before M5): keep it glass.
    if (ior < 1.0)          ior = 1.5;
    if (reflectance <= 0.0) reflectance = 0.5;
    if (dot(transmit, transmit) < 1e-6) transmit = vec3(1.0);

    // Perceptual roughness -> linear a (Filament). Clamp the floor so a "polished"
    // pane still has a finite, stable highlight.
    float perceptualR = clamp(roughness, MIN_PERCEPTUAL_ROUGH, 1.0);
    float a = perceptualR * perceptualR;

    vec3 N = normalize(vNormal);
    vec3 V = normalize(g.camPos.xyz - vWorldPos);
    float NoV = max(dot(N, V), 1e-4);

    // Body / baseColor: bound texture (holo UI / white) * per-object factor * tint.
    vec3 texel = texture(textures[nonuniformEXT(vTexIndex)], vUV).rgb;
    vec3 baseColor = texel * vFactor.rgb * vGlassTint.rgb;

    // F0 (specular reflectance at normal incidence) from IOR/metallic.
    vec3 f0 = computeF0(baseColor, metallic, ior, reflectance);

    // ---- Screen geometry shared by refraction + reflection -----------------
    vec2 screenUV = gl_FragCoord.xy * g.screen.xy;
    bool haveScene = g.screen.w > 0.5;
    bool haveFrost = g.screen.z > 0.5;

    // ---- Ambient (used by the body fill + the reflection sky gradient) ------
    vec3 ambient = cam.ambientCount.rgb;

    // ---- (A) REFRACTED BACKGROUND (the scene seen THROUGH the pane) --------
    // Project N onto the screen plane (camera right/up) for the in-screen bend
    // direction; offset scales with refraction*(ior-1) so a stronger IOR bends more.
    // Sample the sharp copy at a roughness-driven LOD (clamps to mip0 if the copy is
    // single-mip) AND lerp toward the pre-blurred frost copy by roughness, so the
    // scene goes crisp (polished) -> milky (frosted). transmittanceColor lightly
    // tints the background (UE5 thin-translucent), NOT a flat body slab.
    vec3 throughGlass = vec3(0.0);
    if (haveScene) {
        vec2 nScreen = vec2(dot(N, g.camRight.xyz), dot(N, g.camUp.xyz));
        vec2 offUV   = nScreen * refraction * (ior - 1.0) * 2.0;
        vec2 sUV     = clamp(screenUV + offUV, vec2(0.0), vec2(1.0));
        float lod    = perceptualR * MAX_REFLECTION_LOD;
        vec3 sharp   = textureLod(sceneCopy, sUV, lod).rgb;
        vec3 frost   = haveFrost ? textureLod(sceneFrost, sUV, 0.0).rgb : sharp;
        throughGlass = mix(sharp, frost, smoothstep(0.2, 0.9, perceptualR))
                       * mix(vec3(1.0), transmit, 0.65);
    }

    // ---- (B) ENVIRONMENT REFLECTION (the biggest visual win) ----------------
    // Reflect V about N, project to screen, sample the scene copy along it + a
    // sky/ambient gradient fallback; fresnel-weight (roughness-aware) so even a
    // frosted pane keeps a rim. With NO scene copy we still get the sky gradient.
    vec3 R = reflect(-V, N);
    vec2 rScreen = vec2(dot(R, g.camRight.xyz), dot(R, g.camUp.xyz));
    vec3 sky = mix(ambient * 0.6, kSunColor * 1.2, clamp(R.y * 0.5 + 0.5, 0.0, 1.0));
    vec3 reflScene = haveScene
        ? textureLod(sceneCopy, clamp(screenUV + rScreen * 0.25, vec2(0.0), vec2(1.0)),
                     perceptualR * MAX_REFLECTION_LOD).rgb
        : sky;
    vec3 envRefl = haveScene ? mix(sky, reflScene, 0.6) : sky;
    vec3 Fenv = F_SchlickRough(NoV, f0, perceptualR);
    vec3 reflection = envRefl * Fenv;

    // ---- (C) ANALYTIC SPECULAR (sharp Cook-Torrance glints) ----------------
    float ndl    = max(dot(N, kSunDir), 0.0);
    float shadow = sampleShadow(vWorldPos, ndl);
    vec3 specSum = specBRDF(N, V, kSunDir, kSunColor, f0, a) * shadow;

    int n = int(cam.ambientCount.w);
    for (int i = 0; i < n && i < kMaxPointLights; ++i) {
        vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
        float dist = length(toL);
        vec3  L    = toL / max(dist, 0.0001);
        float att  = pointAtten(dist, cam.lights[i].posRange.w);
        specSum   += specBRDF(N, V, L, cam.lights[i].colorPad.rgb, f0, a) * att;
    }
    specSum *= max(specular, 0.0);

    // ---- (D) LIT BODY (the diffuse fill for opaque-ish / tinted panes) ------
    // Standard sun + ambient (+ optional SSAO) diffuse over the dielectric base.
    float ao = 1.0;
    if (ssao.ctrl.x > 0.5) {
        vec2 aoUV = gl_FragCoord.xy * ssao.ctrl.zw;
        float aoSample = texture(ssaoTex, aoUV).r;
        ao = mix(1.0, aoSample, clamp(ssao.ctrl.y, 0.0, 1.0));
    }
    float up = N.y * 0.5 + 0.5;
    vec3 diffuseColor = baseColor * (1.0 - metallic);
    vec3 litBody = diffuseColor * (kSunColor * (0.75 * ndl * shadow)
                                   + ambient * mix(0.85, 1.25, up) * ao);
    // Point-light diffuse on the body.
    for (int i = 0; i < n && i < kMaxPointLights; ++i) {
        vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
        float dist = length(toL);
        vec3  L    = toL / max(dist, 0.0001);
        float pndl = max(dot(N, L), 0.0);
        float att  = pointAtten(dist, cam.lights[i].posRange.w);
        litBody   += diffuseColor * cam.lights[i].colorPad.rgb * (pndl * att);
    }

    // ---- (E) COMPOSITE (energy split) --------------------------------------
    // kS = fresnel reflectance; kT = transmitted energy that survives the reflection.
    vec3  kS = Fenv;
    float kT = 1.0 - max(max(kS.r, kS.g), kS.b);
    float opacity = clamp(vFactor.a, 0.0, 1.0);
    vec3  emissive = vEmissive.rgb * vEmissive.a;   // additive holo glow (feeds bloom)

    vec3 interior = mix(throughGlass, litBody, opacity);
    vec3 color = interior * kT + reflection + specSum + emissive;

    if (haveScene) {
        // Opaque-replace: this fragment SUPPLIES its own (bent) background, so it
        // fully replaces the live HDR pixel (which holds the same, un-bent, scene).
        outColor = vec4(color, 1.0);
    } else {
        // No scene copy: alpha-blend fallback (SRC_ALPHA reveals the live scene).
        // Lift alpha by fresnel so a low-opacity pane still catches light at the rim
        // (never an invisible plane); scale additive light by 1/alpha so the blend
        // (which multiplies by alpha) preserves the intended energy.
        float maxFenv = max(max(Fenv.r, Fenv.g), Fenv.b);
        float outA = clamp(opacity + maxFenv * (1.0 - opacity), 0.0, 1.0);
        vec3 outRGB = litBody + (reflection + specSum + emissive) / max(outA, 0.04);
        outColor = vec4(outRGB, outA);
    }
}
