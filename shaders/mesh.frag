#version 450
#extension GL_EXT_nonuniform_qualifier : require

// GPU-driven mesh fragment shader (Subsystem D + perf-stack E shadows).
//
// Bindless: one large combined-image-sampler array at set0/binding0. The
// per-object texIndex (from the vertex stage) selects the texture; index 0 is
// the built-in 1x1 white default. baseColorFactor rides through from the SSBO
// row (no per-draw UBO).
//
// Shadows (E): the directional sun's lightViewProj (camera UBO, set1/binding1)
// projects the fragment's world position into the shadow map's clip space; a
// 3x3 PCF compare against the depth texture (set2/binding0, a sampler2DShadow
// using hardware depth compare) yields a [0,1] visibility that darkens ONLY the
// directional diffuse term — ambient is kept so shadowed surfaces aren't black.

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
} cam;

// Hardware-compare shadow sampler (depth texture + VK_COMPARE_OP_LESS_OR_EQUAL).
// texture(...) returns the PCF-filtered "fragment is lit" fraction in [0,1].
layout(set = 2, binding = 0) uniform sampler2DShadow shadowMap;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 3) flat in vec4 vFactor;
layout(location = 4) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

const vec3 kSunDir = normalize(vec3(0.4, 1.0, 0.3)); // matches the depth-pass sun

// 3x3 PCF: average 9 hardware-compare taps one texel apart. Returns the lit
// fraction (1 = fully lit, 0 = fully shadowed). ndl drives a slope-scaled bias
// so grazing surfaces (where the depth slope is steep) don't self-shadow (acne)
// while steep biases on flat faces don't cause peter-panning.
float sampleShadow(vec3 worldPos, float ndl) {
    vec4 lc = cam.lightViewProj * vec4(worldPos, 1.0);
    vec3 proj = lc.xyz / lc.w;                  // ortho => w==1, but stay general
    // XY: clip [-1,1] -> UV [0,1]. Z is already [0,1] (GLM_FORCE_DEPTH_ZERO_TO_ONE).
    vec2 uv = proj.xy * 0.5 + 0.5;
    float curDepth = proj.z;

    // Outside the shadow frustum (or behind the far plane) -> treat as fully lit.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || curDepth > 1.0)
        return 1.0;

    // Slope-scaled depth bias, clamped, in light-clip depth units.
    float bias = clamp(0.0015 * tan(acos(clamp(ndl, 0.0, 1.0))), 0.0005, 0.004);
    float refDepth = curDepth - bias;

    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += texture(shadowMap, vec3(uv + vec2(x, y) * texel, refDepth));
    return lit / 9.0;
}

void main() {
    vec3 N = normalize(vNormal);
    float ndl = max(dot(N, kSunDir), 0.0);

    float shadow = sampleShadow(vWorldPos, ndl);
    float ambient = 0.25;
    float diffuse = 0.75 * ndl * shadow;        // sun diffuse, gated by shadow
    float light = ambient + diffuse;

    vec4 albedo = texture(textures[nonuniformEXT(vTexIndex)], vUV) * vFactor;
    outColor = vec4(albedo.rgb * light, albedo.a);
}
