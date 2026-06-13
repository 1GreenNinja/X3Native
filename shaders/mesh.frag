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
    vec4 camPos;                    // xyz = camera world position (PBR view vector)
    vec4 sunDir;                    // xyz = per-scene direction TOWARD the sun (lighting + shadows)
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
    vec4 ctrl;   // x=enabled, y=strength, z=1/screenW, w=1/screenH
    vec4 ibl;    // x=IBL valid(0/1), y=IBL intensity, z=prefilter max mip, w=reserved
} ssao;

// ===========================================================================
// Image-based lighting (IBL), set 4 — split-sum environment reflections.
//   * irradianceCube : diffuse irradiance E(N) (cosine-convolved env)
//   * prefilterCube  : GGX-prefiltered specular radiance, roughness in the mip
//   * brdfLUT        : the scene-independent env-BRDF (scale=.r, bias=.g) vs (NoV,rough)
// Replaces the old flat `ambient*3.4*Fresnel` constant. Baked from the analytic
// sky on the host (see VulkanRenderDevice IBL passes). Gated by ssao.ibl.x so any
// path without a baked environment falls back to the previous flat ambient term.
// ===========================================================================
layout(set = 4, binding = 0) uniform samplerCube irradianceCube;
layout(set = 4, binding = 1) uniform samplerCube prefilterCube;
layout(set = 4, binding = 2) uniform sampler2D   brdfLUT;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 3) flat in vec4 vFactor;
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) flat in vec4 vEmissive;   // rgb = color, a = strength
layout(location = 6) flat in uint vFlags;      // bit0 = TERRAIN, bit1 = GLASS
layout(location = 7) flat in uvec2 vTerrainPack; // x = grass<<16|rock, y = snow<<16|sand
layout(location = 8) flat in uint vNormalTexIndex; // 0 = none (PBR normal map)
layout(location = 9) flat in uint vMrTexIndex;     // 0 = none (metallic-roughness)
layout(location = 10) flat in uint vEmissiveTexIndex; // 0 = none (emissive map)
layout(location = 11) flat in uint vDetailPacked;     // HDRP micro-detail: (uvScale*64<<20)|bindlessIdx

layout(location = 0) out vec4 outColor;

// kSunDir is now PER-SCENE: derived in main() from the Camera UBO (cam.sunDir),
// which the device fills from SkyParams.sunDir. (Was a hardcoded const here.)
// Per-object flag bits (match mesh.vert + VulkanRenderDevice.cpp kFlag*).
const uint FLAG_TERRAIN = 1u;
const uint FLAG_GLASS   = 2u;
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

// ---- PBR helpers (Cook-Torrance GGX). Only the metallic-roughness branch in main()
// calls these; plain meshes (vMrTexIndex == 0) never do, so their shading is unchanged.
const float PI = 3.14159265359;
float D_GGX(float NoH, float a) { float a2 = a * a; float d = (NoH * NoH) * (a2 - 1.0) + 1.0; return a2 / max(PI * d * d, 1e-7); }
float V_SmithGGX(float NoV, float NoL, float a) {
    float k = a * 0.5;
    float gv = NoL * (NoV * (1.0 - k) + k);
    float gl = NoV * (NoL * (1.0 - k) + k);
    return 0.5 / max(gv + gl, 1e-5);
}
vec3 F_Schlick(float u, vec3 f0) { return f0 + (1.0 - f0) * pow(clamp(1.0 - u, 0.0, 1.0), 5.0); }
// Roughness-aware Fresnel (Sebastien Lagarde): rough surfaces keep less grazing
// reflectance than the mirror Schlick term, so IBL specular doesn't over-rim.
vec3 F_SchlickRoughness(float u, vec3 f0, float rough) {
    return f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(clamp(1.0 - u, 0.0, 1.0), 5.0);
}

// Split-sum IBL ambient (Karis/Epic). Returns the combined diffuse + specular
// environment contribution in LINEAR HDR. `perceptualRough` is glTF roughness
// (NOT alpha). `ao` modulates both lobes (specular gets a milder occlusion).
// Falls back to the previous flat ambient*Fresnel constant when no env is baked.
vec3 iblAmbient(vec3 N, vec3 V, vec3 albedo, float metallic, float perceptualRough,
                vec3 F0, float ao, vec3 ambient, float up) {
    if (ssao.ibl.x < 0.5) {
        // FALLBACK (no baked environment): the original engine behaviour exactly —
        // diffuse hemispheric lift + the flat ambient*3.4*Fresnel specular constant.
        float NoV = max(dot(N, V), 1e-4);
        float a   = perceptualRough; a *= a;
        vec3  diff = albedo * (1.0 - metallic);
        vec3  amb  = ambient * mix(0.85, 1.25, up) * ao * diff;
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
    vec3 irradiance = texture(irradianceCube, N).rgb;
    vec3 diffuse = irradiance * albedo;

    // Specular IBL: prefiltered radiance along the reflection vector at mip=rough,
    // scaled by the split-sum env BRDF (F0*scale + bias).
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(prefilterCube, R, perceptualRough * maxMip).rgb;
    vec2 ab = texture(brdfLUT, vec2(NoV, perceptualRough)).rg;
    vec3 specular = prefiltered * (F0 * ab.x + ab.y);

    // Energy ceiling: a near-mirror metal (low roughness, high F0) reflecting a bright
    // environment produces HDR specular so large it clips past ACES to flat white. Soft
    // per-channel Reinhard rolloff: bright reflections compress gracefully toward 1 while
    // dim ones pass through nearly unchanged (x/(1+x): 0.1->0.09, 1->0.5, 4->0.8).
    specular = specular / (1.0 + specular);

    // Specular occlusion: a softer AO on the specular lobe so recesses still darken
    // reflections (full AO would kill them). Diffuse takes the full AO.
    float specAo = clamp(ao + 0.4, 0.0, 1.0);
    return (kD * diffuse * ao + specular * specAo) * intensity;
}
// One light's outgoing radiance factor (Lambert diffuse + GGX spec) * NoL.
vec3 brdf(vec3 N, vec3 V, float NoV, vec3 L, vec3 F0, vec3 diff, float a) {
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
    vec3 F = F_Schlick(VoH, F0);
    vec3 spec = D_GGX(NoH, a) * V_SmithGGX(NoV, NoL, a) * F;
    return ((vec3(1.0) - F) * diff / PI + spec) * NoL;
}
// Perturb the geometry normal by a tangent-space normal map via a derivative TBN
// (no vertex tangents needed). idx = bindless normal-map index.
vec3 perturbNormal(vec3 N, vec3 wp, vec2 uv, uint idx) {
    vec3 t = texture(textures[nonuniformEXT(idx)], uv).xyz * 2.0 - 1.0;
    vec3 dp1 = dFdx(wp), dp2 = dFdy(wp);
    vec2 du1 = dFdx(uv), du2 = dFdy(uv);
    vec3 T = normalize(dp1 * du2.y - dp2 * du1.y);
    vec3 B = -normalize(cross(N, T));
    return normalize(mat3(T, B, N) * t);
}

void main() {
    // GLASS meshes are NOT shaded by the opaque pass — they are drawn in the
    // dedicated transparent glass pass (glass.frag). Discarding here keeps glass
    // out of the opaque color + (with the no-SSAO pipeline) the depth write, so the
    // glass pass composites see-through over the lit scene. The flag is uniform per
    // draw (flat input) so this branch never diverges.
    if ((vFlags & FLAG_GLASS) != 0u) discard;

    vec3 N = normalize(vNormal);
    // PBR normal map (non-terrain): perturb the geometry normal via a derivative TBN.
    if ((vFlags & FLAG_TERRAIN) == 0u && vNormalTexIndex > 0u)
        N = perturbNormal(N, vWorldPos, vUV, vNormalTexIndex);

    // Terrain meshes splat grass/rock/snow/sand by world height+slope; everything
    // else samples its single bindless base texture. Uniform branch (flat input).
    // Bit 31 of the per-object texIndex flags a glTF alphaMode==MASK material (foliage /
    // people billboards); mask it off before sampling, then alpha-cutout below.
    const uint baseIdx     = vTexIndex & 0x3FFFFFFFu;          // bits 30/31 = alpha-mode flags
    const bool alphaCutout = (vTexIndex & 0x80000000u) != 0u;  // bit31 = MASK (cutout)
    const bool alphaBlend  = (vTexIndex & 0x40000000u) != 0u;  // bit30 = BLEND (glass)
    vec4 albedo;
    if ((vFlags & FLAG_TERRAIN) != 0u) {
        albedo = vec4(terrainAlbedo(vWorldPos, N, vTerrainPack), 1.0) * vFactor;
    } else {
        albedo = texture(textures[nonuniformEXT(baseIdx)], vUV) * vFactor;
    }
    // Alpha-cutout: drop the transparent atlas background so sprites aren't opaque quads.
    if (alphaCutout && albedo.a < 0.5) discard;

    // ---- HDRP micro-DETAIL map (vDetailPacked: low 20 bits = bindless idx, high 12 =
    // uvScale*64). HDRP packing R = desat detail albedo (overlay, 0.5 = neutral),
    // B = detail smoothness. Tiled at vUV*uvScale to add fine surface variation the base
    // atlas lacks. (Detail NORMAL is deferred — albedo + roughness give the v1 micro-detail.)
    float detSmoothAdj = 0.0;
    {
        uint dIdx = vDetailPacked & 0xFFFFFu;
        if (dIdx != 0u) {
            float dUv = float(vDetailPacked >> 20) / 64.0;
            vec4 det  = texture(textures[nonuniformEXT(dIdx)], vUV * max(dUv, 0.01));
            albedo.rgb *= clamp(det.r * 2.0, 0.7, 1.3);   // overlay: 0.5 = neutral, kept subtle/safe
            detSmoothAdj = det.b - 0.5;                    // detail smoothness -> roughness nudge below
        }
    }

    // ---- Shared terms: sun shadow, hemispheric ambient, SSAO (both paths use these). ----
    vec3  kSunDir = normalize(cam.sunDir.xyz);   // per-scene sun direction (Camera UBO)
    float ndl    = max(dot(N, kSunDir), 0.0);
    float shadow = sampleShadow(vWorldPos, ndl);
    vec3  ambient = cam.ambientCount.rgb;
    float up = N.y * 0.5 + 0.5;                 // 0 = facing down, 1 = facing up
    float ao = 1.0;
    if (ssao.ctrl.x > 0.5) {
        vec2 aoUV = gl_FragCoord.xy * ssao.ctrl.zw;   // pixel -> [0,1] screen UV
        ao = mix(1.0, texture(ssaoTex, aoUV).r, clamp(ssao.ctrl.y, 0.0, 1.0));
    }
    int nLights = int(cam.ambientCount.w);

    vec3 color;
    if (vMrTexIndex == 0u) {
        // ---- DIELECTRIC path (no MR map). Direct sun + point lights as before; the
        // ambient term is now SPLIT-SUM IBL so plain white cladding/floors finally
        // reflect the environment (dielectric: metallic=0, satin roughness ~0.5).
        // Falls back to the old flat ambient when no env is baked. ----
        vec3 lighting = kSunColor * (0.75 * ndl * shadow);
        for (int i = 0; i < nLights && i < kMaxPointLights; ++i) {
            vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
            float dist = length(toL);
            vec3  L    = toL / max(dist, 0.0001);
            lighting  += cam.lights[i].colorPad.rgb * (max(dot(N, L), 0.0) * pointAtten(dist, cam.lights[i].posRange.w));
        }
        vec3 Vd = normalize(cam.camPos.xyz - vWorldPos);
        const float kDielectricRough = 0.5;   // satin clad/floor default
        vec3 F0d = vec3(0.04);
        color = albedo.rgb * lighting
              + iblAmbient(N, Vd, albedo.rgb, 0.0, kDielectricRough, F0d, ao, ambient, up);
    } else {
        // ---- PBR metallic-roughness (Cook-Torrance GGX). glTF MR: B=metallic, G=roughness. ----
        vec3  mr       = texture(textures[nonuniformEXT(vMrTexIndex)], vUV).rgb;
        float metallic = mr.b;
        float pRough   = clamp(mr.g - detSmoothAdj * 0.4, 0.045, 1.0);  // perceptual roughness (for IBL) + detail-smoothness nudge
        float a        = pRough; a *= a;                         // -> GGX alpha (direct lights)
        vec3  F0       = mix(vec3(0.04), albedo.rgb, metallic);
        vec3  diff     = albedo.rgb * (1.0 - metallic);
        vec3  V        = normalize(cam.camPos.xyz - vWorldPos);
        float NoV      = max(dot(N, V), 1e-4);
        vec3  Lo = brdf(N, V, NoV, kSunDir, F0, diff, a) * kSunColor * shadow;        // sun (shadowed)
        for (int i = 0; i < nLights && i < kMaxPointLights; ++i) {                    // point lights
            vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
            float dist = length(toL);
            vec3  L    = toL / max(dist, 0.0001);
            Lo += brdf(N, V, NoV, L, F0, diff, a) * cam.lights[i].colorPad.rgb * pointAtten(dist, cam.lights[i].posRange.w);
        }
        // Image-based lighting: SPLIT-SUM diffuse irradiance + GGX-prefiltered specular
        // from the analytic-sky environment cube, so metals reflect the sky and
        // dielectric floors/glass get real specular env. Replaces the old flat
        // ambient diffuse + ambient*3.4*Fresnel constant (kept as the no-env fallback).
        Lo += iblAmbient(N, V, albedo.rgb, metallic, pRough, F0, ao, ambient, up);
        color = Lo;
    }

    // ---- Emissive: per-object HDR radiance on top (glows even in shadow; feeds bloom). ----
    vec3 emis = vEmissive.rgb;
    if (vEmissiveTexIndex > 0u) emis *= texture(textures[nonuniformEXT(vEmissiveTexIndex)], vUV).rgb;  // emissive map gates WHERE it glows (edge strips)
    color += emis * vEmissive.a;
    // BLEND (glass): Unity glass mats often have baseColorFactor.a=0 -> invisible under straight
    // alpha-over. Floor the opacity + add a fresnel grazing term so glass reads as a translucent,
    // reflective pane. Gated on vFactor.a<0.99 so a=1 BLEND overlays (screens) stay solid.
    float outA = albedo.a;
    if (alphaBlend && vFactor.a < 0.99) {
        float baseOp = mix(0.10, 0.32, clamp(vFactor.a, 0.0, 1.0));   // mostly SEE-THROUGH (was washing white)
        vec3  Vv     = normalize(cam.camPos.xyz - vWorldPos);
        float fres   = pow(1.0 - max(dot(N, Vv), 0.0), 5.0);
        outA = clamp(baseOp + fres * 0.35, 0.0, 1.0);                 // edges firmer, face near-clear
        color += kSunColor * fres * 0.06;                            // subtle grazing rim only
    }
    outColor = vec4(color, outA);   // HDR linear; tonemap is in composite.frag
}
