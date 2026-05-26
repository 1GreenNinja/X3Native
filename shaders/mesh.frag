#version 450
#extension GL_EXT_nonuniform_qualifier : require

// GPU-driven mesh fragment shader (Subsystem D + perf-stack E shadows + forward
// point lights).
//
// Bindless: one large combined-image-sampler array at set0/binding0. The
// per-object texIndex (from the vertex stage) selects the texture; index 0 is
// the built-in 1x1 white default. baseColorFactor rides through from the SSBO
// row (no per-draw UBO).
//
// Lighting model (all additive, accumulated in LINEAR light, output as HDR):
//   * Directional sun (kSunDir): diffuse gated by the shadow map (perf-stack E).
//   * Hemispheric ambient: a small up/down-blended constant lift (from the UBO)
//     so shadowed surfaces / back-faces aren't pure black.
//   * Forward point lights: a bounded loop over the per-frame light array (UBO,
//     set1/binding1) with smooth windowed distance attenuation — the corridor's
//     Light_A ceiling fixtures. Unshadowed (the single shadow map is the sun's).
//   * Emissive: a per-object emissive radiance (from the SSBO row) added on top
//     so light fixtures / strips are bright HDR sources that feed bloom.
//
// HDR PIPELINE: this shader renders to an R16G16B16A16_SFLOAT scene target in
// LINEAR light and does NOT tonemap. The ACES filmic curve now runs ONCE in the
// composite pass (shaders/composite.frag), after bloom is added. (Previously the
// tonemap was applied per-fragment here; it was moved out for the HDR + bloom
// pipeline.)
//
// Shadows (E): the directional sun's lightViewProj (UBO, set1/binding1) projects
// the fragment's world position into the shadow map's clip space; a 3x3 PCF
// compare against the depth texture (set2/binding0, a sampler2DShadow using
// hardware depth compare) yields a [0,1] visibility that darkens ONLY the
// directional diffuse term — ambient + point lights are kept so shadowed
// surfaces aren't black.

layout(set = 0, binding = 0) uniform sampler2D textures[];

// One forward point light (matches GpuPointLight in VulkanRenderDevice.cpp /
// PointLight in IRenderDevice.h). std140: two vec4s, 32 bytes.
struct PointLight {
    vec4 posRange;   // xyz = world position, w = range (meters)
    vec4 colorPad;   // rgb = linear color * intensity, a = unused
};

const int kMaxPointLights = 64;

// Per-frame UBO. Must match FrameUBO (VulkanRenderDevice.cpp): camera viewProj +
// sun lightViewProj, then the ambient/count header and the point-light array.
layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 ambientCount;              // rgb = ambient color, w = active light count
    PointLight lights[kMaxPointLights];
} cam;

// Hardware-compare shadow sampler (depth texture + VK_COMPARE_OP_LESS_OR_EQUAL).
// texture(...) returns the PCF-filtered "fragment is lit" fraction in [0,1].
layout(set = 2, binding = 0) uniform sampler2DShadow shadowMap;

// Screen-space ambient occlusion (set3): the blurred half-res AO texture (R8,
// 1 = unoccluded, 0 = occluded), sampled at the fragment's screen UV. AO darkens
// ONLY the ambient/indirect term (NOT the direct sun or point lights) so corners,
// crevices + contact points get soft occlusion without crushing lit surfaces.
// binding1 (ctrl): x = enabled (0/1), y = strength (lerps the applied AO between
// 1.0 and the sampled value), z = 1/screenW, w = 1/screenH (pixel -> UV).
layout(set = 3, binding = 0) uniform sampler2D ssaoTex;
layout(set = 3, binding = 1) uniform SsaoControl {
    vec4 ctrl;
} ssao;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 3) flat in vec4 vFactor;
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) flat in vec4 vEmissive;   // rgb = color, a = strength
layout(location = 6) flat in uint vFlags;      // bit0 = TERRAIN, bit1 = GLASS
layout(location = 7) flat in uvec2 vTerrainPack; // x = grass<<16|rock, y = snow<<16|sand

layout(location = 0) out vec4 outColor;

// Per-object flag bits (match mesh.vert + VulkanRenderDevice.cpp kFlag*).
const uint FLAG_TERRAIN = 1u;
const uint FLAG_GLASS   = 2u;

const vec3 kSunDir   = normalize(vec3(0.4, 1.0, 0.3)); // matches the depth-pass sun
const vec3 kSunColor = vec3(1.0, 0.97, 0.92);          // slightly warm white sun

// ===========================================================================
// Terrain material splat (open-world ground). Procedural height + slope blend of
// four tiling detail textures (grass / rock / snow / sand) keyed off the
// fragment's WORLD position + WORLD surface normal, with a little value noise to
// break the tiling. Clean-room: built from the public height/slope-splat +
// triplanar mapping technique (GPU Gems terrain, the standard height/slope splat
// articles) — no engine source consulted. Only runs when vTerrainFlag != 0; all
// other meshes skip this entirely and shade exactly as before.
//
// CONSTANTS (tunables): these mirror the host world config — kSeaLevel matches
// `--world ocean`'s seaLevel (14 m) and the band heights are fractions of the
// default heightScale (55 m). The sand band sits at the shoreline so it meets the
// ocean cleanly; grass is low+flat; rock is steep slope; snow caps high peaks.
// ---------------------------------------------------------------------------
// NOTE these are calibrated to the actual streamed-world field (heightScale 55 m;
// sampled height ~6..50 m, avg ~31 m; the fBm hills are GENTLE — slope normal.y
// is almost never below ~0.85, so the rock thresholds sit high on purpose so the
// steeper hillsides actually read as rock instead of rock never appearing).
const float kSeaLevel    = 14.0;   // world Y of the ocean surface (matches host)
const float kSandTop     = 18.0;   // sand fades out a few m above sea level
const float kSnowBottom  = 36.0;   // snow begins on the high ground
const float kSnowFull    = 47.0;   // fully snow by here
const float kSlopeRockLo = 0.90;   // normal.y at/below this -> full rock (steep)
const float kSlopeRockHi = 0.965;  // normal.y at/above this -> no rock (flat)
const float kDetailScale = 0.18;   // world-space detail tiling (cycles / meter)
const float kMacroScale  = 0.012;  // large-scale tint variation frequency

// Cheap hash-based value noise on world XZ (self-contained; matches the CPU
// terrain's value-noise idea so the breakup reads consistent). Returns [0,1).
float thash(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}
float tnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = thash(i);
    float b = thash(i + vec2(1.0, 0.0));
    float c = thash(i + vec2(0.0, 1.0));
    float d = thash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Sample a bindless detail texture by world XZ (top-down planar projection) at
// the detail tiling scale. World-space UVs => tiles seam seamlessly across the
// streamed terrain (no per-tile UV reset).
vec3 detailXZ(uint idx, vec2 worldXZ) {
    return texture(textures[nonuniformEXT(idx)], worldXZ * kDetailScale).rgb;
}

// Triplanar sample for steep rock so vertical cliffs don't get the smeared
// top-down stretch. Blends the three world-axis planar projections by |normal|.
vec3 triplanar(uint idx, vec3 wpos, vec3 wn) {
    vec3 an = abs(wn);
    an = an / max(an.x + an.y + an.z, 1e-4);
    vec3 cx = texture(textures[nonuniformEXT(idx)], wpos.zy * kDetailScale).rgb;
    vec3 cy = texture(textures[nonuniformEXT(idx)], wpos.xz * kDetailScale).rgb;
    vec3 cz = texture(textures[nonuniformEXT(idx)], wpos.xy * kDetailScale).rgb;
    return cx * an.x + cy * an.y + cz * an.z;
}

// Procedural height+slope splat. Returns the blended terrain albedo in linear-ish
// sRGB (the detail textures are stored sRGB so the array already linearises them).
vec3 terrainAlbedo(vec3 wpos, vec3 wn, uvec2 pack) {
    uint grassIdx = pack.x >> 16;
    uint rockIdx  = pack.x & 0xFFFFu;
    uint snowIdx  = pack.y >> 16;
    uint sandIdx  = pack.y & 0xFFFFu;

    float h     = wpos.y;
    float slope = clamp(wn.y, 0.0, 1.0);     // 1 = flat ground, 0 = vertical

    // A bit of world noise to wobble the band boundaries so they don't read as
    // perfectly horizontal contour lines.
    float n   = tnoise(wpos.xz * kMacroScale) - 0.5;   // [-0.5,0.5]
    float hN  = h + n * 6.0;                            // height jittered by noise

    // ---- Base sample the four materials (world-space UVs) ----
    vec3 grass = detailXZ(grassIdx, wpos.xz);
    vec3 sand  = detailXZ(sandIdx,  wpos.xz);
    vec3 snow  = detailXZ(snowIdx,  wpos.xz);
    // Rock uses triplanar so cliffs aren't stretched.
    vec3 rock  = triplanar(rockIdx, wpos, wn);

    // ---- Height bands (smoothstep transitions, never hard) ----
    // Start as grass everywhere, then layer sand low, snow high, rock on slope.
    vec3 albedo = grass;

    // Sand/dirt shoreline band: strongest right at/below sea level, fading out by
    // kSandTop so it meets the ocean cleanly at the waterline.
    float sandBand = 1.0 - smoothstep(kSeaLevel - 2.0, kSandTop, hN);
    albedo = mix(albedo, sand, clamp(sandBand, 0.0, 1.0));

    // Snow cap on the high ground.
    float snowBand = smoothstep(kSnowBottom, kSnowFull, hN);
    albedo = mix(albedo, snow, clamp(snowBand, 0.0, 1.0));

    // ---- Slope rock: overrides whatever band where the surface is steep. The
    // thresholds are high (see note above) because this terrain's hillsides are
    // gentle; rock fades in below kSlopeRockHi and is full by kSlopeRockLo. ----
    float rockBand = 1.0 - smoothstep(kSlopeRockLo, kSlopeRockHi, slope);
    // Wobble the rock edge with noise so it isn't a clean contour line.
    rockBand = clamp(rockBand + n * 0.18, 0.0, 1.0);
    albedo = mix(albedo, rock, rockBand);

    // Subtle macro tint variation so large flat areas aren't a flat colour.
    float macro = tnoise(wpos.xz * (kMacroScale * 0.5));
    albedo *= mix(0.88, 1.10, macro);

    return albedo;
}

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

// Smooth windowed point-light attenuation: bright near the source, smoothly
// reaching exactly 0 at `range` (no hard cutoff seam, no unbounded 1/d^2 spike).
//   window = clamp(1 - (d/range)^4, 0, 1)^2   (UE4-style range falloff)
//   falloff = window / (d^2 + 1)              (bounded inverse-square-ish core)
float pointAtten(float dist, float range) {
    float t = dist / max(range, 0.0001);
    float w = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    w *= w;
    return w / (dist * dist + 1.0);
}

void main() {
    // GLASS meshes are NOT shaded by the opaque pass — they are drawn in the
    // dedicated transparent glass pass (glass.frag). Discarding here keeps glass
    // out of the opaque color + (with the no-SSAO pipeline) the depth write, so the
    // glass pass composites see-through over the lit scene. The flag is uniform per
    // draw (flat input) so this branch never diverges.
    if ((vFlags & FLAG_GLASS) != 0u) discard;

    vec3 N = normalize(vNormal);

    // Terrain meshes (flagged in the SSBO) splat grass/rock/snow/sand by world
    // height + slope; everything else samples its single bindless texture exactly
    // as before. The branch is uniform per draw (flat input), so no divergence.
    vec4 albedo;
    if ((vFlags & FLAG_TERRAIN) != 0u) {
        albedo = vec4(terrainAlbedo(vWorldPos, N, vTerrainPack), 1.0) * vFactor;
    } else {
        albedo = texture(textures[nonuniformEXT(vTexIndex)], vUV) * vFactor;
    }

    // ---- Directional sun (shadow-gated diffuse) ----
    float ndl = max(dot(N, kSunDir), 0.0);
    float shadow = sampleShadow(vWorldPos, ndl);
    vec3 lighting = kSunColor * (0.75 * ndl * shadow);

    // ---- Hemispheric ambient: blend a slightly warmer "up" tint with the cooler
    // base ambient by the surface's vertical facing so floors/ceilings differ. ----
    vec3 ambient = cam.ambientCount.rgb;
    float up = N.y * 0.5 + 0.5;                 // 0 = facing down, 1 = facing up

    // SSAO modulates ONLY this ambient/indirect term (the sun + point lights are
    // direct light and stay full-strength). Sample the blurred AO at this
    // fragment's screen UV; `strength` lerps the effect (1 = full AO, 0 = off).
    float ao = 1.0;
    if (ssao.ctrl.x > 0.5) {
        vec2 aoUV = gl_FragCoord.xy * ssao.ctrl.zw;   // pixel -> [0,1] screen UV
        float aoSample = texture(ssaoTex, aoUV).r;
        ao = mix(1.0, aoSample, clamp(ssao.ctrl.y, 0.0, 1.0));
    }
    lighting += ambient * mix(0.85, 1.25, up) * ao;

    // ---- Forward point lights (Light_A ceiling fixtures) ----
    int n = int(cam.ambientCount.w);
    for (int i = 0; i < n && i < kMaxPointLights; ++i) {
        vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
        float dist = length(toL);
        vec3  L    = toL / max(dist, 0.0001);
        float pndl = max(dot(N, L), 0.0);
        float att  = pointAtten(dist, cam.lights[i].posRange.w);
        lighting  += cam.lights[i].colorPad.rgb * (pndl * att);
    }

    // ---- Emissive: a per-object HDR radiance added on top (light fixtures /
    // strips). Independent of incoming light so a fixture glows even in shadow;
    // multiplied into HDR range by its strength so it drives the bloom chain. ----
    vec3 color = albedo.rgb * lighting;
    color += vEmissive.rgb * vEmissive.a;

    // HDR output in LINEAR light — NO tonemap here (moved to composite.frag).
    outColor = vec4(color, albedo.a);
}
