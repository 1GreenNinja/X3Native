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
//   M3 (fresnel + specular shimmer): later.
//   M4 (roughness / frost): later.
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
//            Mip-chained for the frost lookup (M4); M2 samples mip 0.
// binding 1: per-frame control (camera pos + time + screen->UV + dev overrides +
//            camera right/up for the screen-space normal projection).
layout(set = 4, binding = 0) uniform sampler2D sceneCopy;
layout(set = 4, binding = 1) uniform GlassControl {
    vec4 camPos;     // xyz = camera world pos, w = time
    vec4 screen;     // x = 1/W, y = 1/H, z = maxMip, w = sceneCopyValid (0/1)
    vec4 ctrl;       // x = refractScale, y = roughAdd, z = specScale, w = overrideOn
    vec4 camRight;   // xyz = camera RIGHT axis (world)
    vec4 camUp;      // xyz = camera UP axis (world)
} g;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 3) flat in vec4 vFactor;       // rgb tint*texel, a = OPACITY (glass)
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) flat in vec4 vEmissive;     // rgb = color, a = strength
layout(location = 6) flat in uint vFlags;        // bit0 = TERRAIN, bit1 = GLASS
layout(location = 7) flat in uvec2 vTerrainPack; // unused for glass
layout(location = 8) flat in vec4 vGlassParams;  // x = refraction, y = roughness, z = specular
layout(location = 9) flat in vec4 vGlassTint;    // rgb = tint

layout(location = 0) out vec4 outColor;

const uint FLAG_GLASS = 2u;

const vec3 kSunDir   = normalize(vec3(0.4, 1.0, 0.3));
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

void main() {
    // Only GLASS-flagged fragments belong to this pass; everything else is opaque
    // and already rendered.
    if ((vFlags & FLAG_GLASS) == 0u) discard;

    vec3 N = normalize(vNormal);

    // ---- Per-object glass material (M2-M4), with optional live dev override. ----
    // ctrl.w == 1 -> scale/add the authored material (r_glass_* scrubbing).
    float refraction = vGlassParams.x;
    if (g.ctrl.w > 0.5) refraction *= g.ctrl.x;

    // Body color: the bound texture (holo UI / white) tinted by the glass color.
    // vFactor.rgb carries the per-object baseColor; vGlassTint.rgb is the glass
    // tint. Treat white tint (default) as colorless.
    vec3 texel = texture(textures[nonuniformEXT(vTexIndex)], vUV).rgb;
    vec3 tint  = vGlassTint.rgb;
    vec3 body  = texel * vFactor.rgb * tint;

    // ---- M2: screen-space REFRACTION -------------------------------------
    // Project the world-space surface normal onto the screen plane (camera right/up)
    // to get the in-screen bend direction, then sample the scene-color copy at the
    // refraction-offset UV. The copy is the scene as it looked BEFORE this glass was
    // drawn (you can't sample + write the live HDR target in one pass).
    vec2 screenUV = gl_FragCoord.xy * g.screen.xy;
    vec3 refractedScene = vec3(0.0);
    bool haveScene = g.screen.w > 0.5;
    if (haveScene) {
        vec2 nScreen = vec2(dot(N, g.camRight.xyz), dot(N, g.camUp.xyz));
        vec2 offUV   = nScreen * refraction;
        vec2 sampUV  = clamp(screenUV + offUV, vec2(0.0), vec2(1.0));
        refractedScene = textureLod(sceneCopy, sampUV, 0.0).rgb;
    }

    // ---- Lighting (same model as the opaque mesh) — used for the glass BODY ----
    float ndl = max(dot(N, kSunDir), 0.0);
    float shadow = sampleShadow(vWorldPos, ndl);
    vec3 lighting = kSunColor * (0.75 * ndl * shadow);

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
    }

    // ---- Compose the glass colour ----------------------------------------
    vec3 litBody = body * lighting;
    litBody += vEmissive.rgb * vEmissive.a;   // holo glow kept (feeds bloom)

    float opacity = clamp(vFactor.a, 0.0, 1.0);

    if (haveScene) {
        // SCREEN-SPACE refraction path: we have a copy of the scene behind the glass,
        // so this fragment SUPPLIES its own background (the BENT scene) rather than
        // letting the alpha blend reveal the un-bent live scene. Compose in the
        // shader: the refracted scene tinted by the glass colour, with the lit body
        // mixed in by opacity (clear pane -> mostly bent scene; opaque -> mostly
        // body). Output alpha = 1 so the composed result fully replaces the live HDR
        // pixel (which holds the SAME, but un-bent, scene). This is what makes the
        // refraction actually visible.
        vec3 throughGlass = refractedScene * mix(vec3(1.0), tint, 0.5);
        vec3 color = mix(throughGlass, litBody, opacity);
        outColor = vec4(color, 1.0);
    } else {
        // No scene copy (target failed / disabled): fall back to the M1 alpha
        // see-through — the SRC_ALPHA blend reveals the live scene behind.
        outColor = vec4(litBody, opacity);
    }
}
