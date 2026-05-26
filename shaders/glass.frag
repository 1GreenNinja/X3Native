#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Translucent GLASS fragment shader — the transparent pass companion to mesh.frag
// (design spec docs/superpowers/specs/2026-05-25-glass-material-design.md).
//
// Drawn in a dedicated post-opaque pass: depth-tested LESS_OR_EQUAL against the
// opaque depth, depth-write OFF, alpha-blended (SRC_ALPHA / ONE_MINUS_SRC_ALPHA)
// into the SAME linear HDR scene the opaque pass produced. It shares the mesh
// pipeline's vertex shader (mesh.vert) + descriptor-set layout (bindless textures
// set0, camera UBO+SSBO set1, shadow map set2, SSAO set3), so it reads the exact
// same per-object payload.
//
// MILESTONE M1 (ALPHA SEE-THROUGH): a GLASS fragment is lit like the opaque mesh
// (sun + ambient + point lights + emissive) and output with alpha = opacity (from
// baseColorFactor.a, which drawMeshGlass set from GlassMaterial.opacity). NON-glass
// fragments DISCARD here (they belong to the opaque pass). Refraction (M2), fresnel
// + specular shimmer (M3) and roughness/frost (M4) layer on top in later milestones.

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

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 3) flat in vec4 vFactor;       // rgb tint*texel, a = OPACITY (glass)
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) flat in vec4 vEmissive;     // rgb = color, a = strength
layout(location = 6) flat in uint vFlags;        // bit0 = TERRAIN, bit1 = GLASS
layout(location = 7) flat in uvec2 vTerrainPack; // unused for glass

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
    // and already rendered. Discard keeps the glass pass from disturbing opaque
    // pixels even if a non-glass draw is ever issued through this pipeline.
    if ((vFlags & FLAG_GLASS) == 0u) discard;

    vec3 N = normalize(vNormal);

    // Body color: the bound texture (holo UI / white) tinted by the glass color.
    // vFactor.rgb already carries the tint*texel multiply; .a is the opacity.
    vec4 albedo = texture(textures[nonuniformEXT(vTexIndex)], vUV) * vec4(vFactor.rgb, 1.0);

    // ---- Lighting (same model as the opaque mesh) ----
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

    vec3 color = albedo.rgb * lighting;
    color += vEmissive.rgb * vEmissive.a;   // holo glow kept (feeds bloom)

    // M1: see-through via the opacity alpha. The pipeline pre-multiplies nothing —
    // it blends SRC_ALPHA/ONE_MINUS_SRC_ALPHA, so a low alpha lets the lit scene
    // behind show through. clamp keeps it in [0,1].
    float opacity = clamp(vFactor.a, 0.0, 1.0);
    outColor = vec4(color, opacity);
}
