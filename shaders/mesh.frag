#version 460
// (bumped 450 -> 460 for the RT_SHADOWS variant below: GL_EXT_ray_query needs
// GLSL 4.60. For the plain variant this changes ONLY the OpSource debug line —
// the generated code is unchanged.)
#extension GL_EXT_nonuniform_qualifier : require
#ifdef RT_SHADOWS
// RAY-TRACED SOFT SHADOWS (r_rtshadows) — compiled ONLY into mesh_rt.frag.spv
// (-DRT_SHADOWS=1, SPIR-V 1.4, bound only on ray-query devices). The plain
// mesh.frag.spv build has none of this code, so r_rtshadows 0 / non-RT devices
// are byte-for-byte the existing pipeline. CLEAN-ROOM: built from the Vulkan
// 1.3 spec + the public GLSL_EXT_ray_query extension spec (the rtao.comp /
// refl.comp pattern) and standard soft-shadow cone-sampling math.
#extension GL_EXT_ray_query : require
#endif

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
    vec4 ctrl;        // x=enabled, y=strength, z=1/screenW, w=1/screenH
    vec4 ibl;         // x=IBL valid(0/1), y=IBL intensity, z=prefilter max mip, w=metal ambient-spec floor strength (r_metalambient)
    vec4 refl;        // x=reflections active (0/1), y=intensity, z/w=reserved (SSR/RT reflection pass, r_ssr)
    // ---- DDGI probe-grid irradiance (r_ddgi; ray-query hardware only) ----
    vec4 ddgiCtrl;    // x=active (0/1), y=intensity (warm-up ramped), z=debug mode (0/1/2), w=self-shadow bias scale
    vec4 ddgiOrigin;  // xyz = probe-grid min corner (world), w = visMaxDist (m)
    vec4 ddgiSpacing; // xyz = probe spacing (m), w = unused
    vec4 ddgiCounts;  // xyz = probe counts (as float), w = unused
    // ---- RT soft shadows (r_rtshadows; read ONLY by the RT_SHADOWS variant) ----
    vec4 rtsh0;       // x = tier (0=off,1=sun,2=sun+points), y = tan(sun angular radius), z = max point shadow rays K, w = point light source radius (m)
    vec4 rtsh1;       // x = frame seed (per-frame jitter rotation; 0 when TAA is off), yzw = reserved
} ssao;
// Screen-traced / ray-traced reflection buffer (set3/binding2, half- or full-res
// RGBA16F): rgb = reflected radiance from the REFLECTION pass (refl.comp — SSR
// march against the depth buffer sampling LAST frame's lit scene, with an
// optional ray-query fallback), a = confidence [0,1]. Sampled at the fragment's
// screen UV (the ssaoTex pattern) and blended into the IBL specular below,
// gated by ssao.refl.x — when 0 this texture is never read and the IBL path is
// byte-for-byte the pre-reflections math.
layout(set = 3, binding = 2) uniform sampler2D reflTex;
// DDGI probe atlases (set3 bindings 3/4, r_ddgi). Octahedral-encoded per-probe
// irradiance (8x8 tiles: 6x6 interior + 1px border, RGBA16F) and mean/mean^2
// visibility depth (16x16 tiles: 14x14 + border, RG16F), produced by the
// ddgi_rays/ddgi_update compute passes against the scene TLAS. Sampled ONLY
// when ssao.ddgiCtrl.x is 1 (a real ray-query-tier frame); on every other
// path these bindings point at a layout-valid placeholder and are never read
// — the existing ambient/IBL math is byte-for-byte unchanged.
layout(set = 3, binding = 3) uniform sampler2D ddgiIrrTex;
layout(set = 3, binding = 4) uniform sampler2D ddgiVisTex;
#ifdef RT_SHADOWS
// Scene TLAS (set3/binding5 — present in the set LAYOUT only on ray-query
// devices; the plain mesh.frag never declares it). Same acceleration structure
// the RT-AO / RT-reflections / DDGI passes trace. Written once the first TLAS
// build lands; the host binds this pipeline variant only on frames where the
// TLAS descriptor is valid.
layout(set = 3, binding = 5) uniform accelerationStructureEXT rtShadowTlas;
#endif

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
// GLASS material varyings ride locations 12/13 (glass.frag reads both). The opaque
// mesh.frag only needs vGlassParams.w — repurposed as a per-object METALLIC-CLAMP
// scale for the room-dressing kit props (0 = no clamp -> treat as 1.0). See the
// BLACK-PROP FIX in the PBR path below + IRenderDevice::drawMeshPBR(metallicScale).
layout(location = 12) flat in vec4 vGlassParams;

layout(location = 0) out vec4 outColor;

// kSunDir is now PER-SCENE: derived in main() from the Camera UBO (cam.sunDir),
// which the device fills from SkyParams.sunDir. (Was a hardcoded const here.)
// Per-object flag bits (match mesh.vert + VulkanRenderDevice.cpp kFlag*).
const uint FLAG_TERRAIN = 1u;
const uint FLAG_GLASS   = 2u;
// CLEARCOAT (car paint): a second fixed-F0 (0.04) specular lobe over the base
// layer. Params ride the SPARE terrain-pack1 lane (vTerrainPack.x — mutually
// exclusive with TERRAIN): low byte = intensity*255, next byte = roughness*255.
const uint FLAG_CLEARCOAT = 4u;
// CANON: SHIPS ARE SELF-LIT (Star Trek convention). A hull must never render as a
// black silhouette just because the star is on its far side. The intensity byte
// rides vTerrainPack.y (the spare lane; clearcoat owns .x, terrain owns both).
const uint FLAG_SHIP_SELFLIT = 8u;
// FOLIAGE (trees/vegetation): wrap the diffuse so the canopy's away-side isn't flat
// black, + a warm back-translucency lobe so the low sun glows THROUGH the leaves.
const uint FLAG_FOLIAGE = 16u;
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
// NOTE (W8-3) recalibrated to the CANONICAL WORLD FIELD (terrain.cpp
// worldFeatures): base rolling field 0..55 m with macro relief, plus 4 mountain
// ranges 7-10 km out whose peaks reach ~400-500 m. Bands are absolute world Y:
//   * SAND is a SHORELINE material only — gated by proximity to the offshore
//     ocean basin (kShoreXZ, matches terrain.cpp kBasinCx/Cz) so inland lows and
//     the flattened city/facility pads don't read as beach.
//   * SNOW lives on the high ranges only (kSnowBottom 180 — the base field can
//     never reach it) and prefers flatter ground (it slides off cliffs).
//   * ALPINE band: grass gives way to rock with altitude well below the snowline.
//   * Slope-rock thresholds unchanged — on real mountainsides normal.y drops far
//     below 0.90, so slopes saturate to full rock naturally.
//   * Range-theme tints (kVolcanoXZ / kMesaXZ / kCrystalXZ, matched to terrain.cpp
//     kRanges band midpoints): E volcanic = dark basalt rock + no snow; S mesa =
//     warm sandstone rock; W highlands = mossier grass.
const float kSeaLevel    = 4.0;    // world Y of the basin water surface (ocean_base)
const float kSandTop     = 16.0;   // beach fades out by here (near the basin only)
const float kSnowBottom  = 180.0;  // snow begins (mountain shoulders)
const float kSnowFull    = 265.0;  // fully snow by here (high peaks)
const float kAlpineLo    = 75.0;   // grass starts yielding to rock
const float kAlpineHi    = 140.0;  // fully rock by here (below the snow line)
const float kSlopeRockLo = 0.90;   // normal.y at/below this -> full rock (steep)
const float kSlopeRockHi = 0.965;  // normal.y at/above this -> no rock (flat)
const float kDetailScale = 0.18;   // world-space detail tiling (cycles / meter)
const float kMacroScale  = 0.012;  // large-scale tint variation frequency
const vec2  kShoreXZ     = vec2( 1100.0, -1350.0);  // ocean basin center
const vec2  kVolcanoXZ   = vec2( 9200.0,   250.0);  // E range band midpoint
const vec2  kMesaXZ      = vec2(  350.0, -9000.0);  // S range band midpoint
const vec2  kCrystalXZ   = vec2(-8600.0,  -100.0);  // W range band midpoint

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

    // ---- Range-theme masks (biome tints keyed off world position; centers
    // match terrain.cpp's kRanges band midpoints) ----
    float volc  = 1.0 - smoothstep(1400.0, 3200.0, distance(wpos.xz, kVolcanoXZ));
    float mesa  = 1.0 - smoothstep(2400.0, 4200.0, distance(wpos.xz, kMesaXZ));
    float moss  = 1.0 - smoothstep(1600.0, 3400.0, distance(wpos.xz, kCrystalXZ));

    // ---- Base sample the four materials (world-space UVs) ----
    vec3 grass = detailXZ(grassIdx, wpos.xz);
    vec3 sand  = detailXZ(sandIdx,  wpos.xz);
    vec3 snow  = detailXZ(snowIdx,  wpos.xz);
    // Rock uses triplanar so cliffs aren't stretched.
    vec3 rock  = triplanar(rockIdx, wpos, wn);

    // Theme the materials: dark basalt in the volcanic east, warm sandstone in
    // the southern mesas, mossier green in the western highlands.
    rock  = mix(rock, rock * vec3(0.42, 0.34, 0.32), volc);
    rock  = mix(rock, rock * vec3(1.15, 0.92, 0.68), mesa);
    grass = mix(grass, grass * vec3(0.80, 1.08, 0.72), moss);

    // ---- Height bands (smoothstep transitions, never hard) ----
    // Start as grass everywhere, then layer beach sand at the basin shoreline,
    // alpine rock with altitude, snow on the peaks, rock on slope.
    vec3 albedo = grass;

    // Beach band: strongest right at/below the basin waterline, fading out by
    // kSandTop — and gated to the SHORE (inland lows / city pads are not beach).
    float shore = 1.0 - smoothstep(950.0, 1500.0, distance(wpos.xz, kShoreXZ));
    float sandBand = (1.0 - smoothstep(kSeaLevel - 2.0, kSandTop, hN)) * shore;
    albedo = mix(albedo, sand, clamp(sandBand, 0.0, 1.0));

    // Alpine band: grass yields to rock with altitude (below the snow line).
    float alpine = smoothstep(kAlpineLo, kAlpineHi, hN);
    albedo = mix(albedo, rock, alpine * 0.9);

    // Snow cap on the high ranges: prefers flatter ground (slides off cliffs);
    // suppressed over the volcanic east (basalt stays dark).
    float snowBand = smoothstep(kSnowBottom, kSnowFull, hN)
                   * smoothstep(0.55, 0.80, slope)
                   * (1.0 - volc);
    albedo = mix(albedo, snow, clamp(snowBand, 0.0, 1.0));

    // ---- Slope rock: overrides whatever band where the surface is steep. The
    // thresholds sit high so even the gentle base field's steeper hillsides read
    // as rock; real mountainsides drop normal.y far below 0.90 and saturate. ----
    float rockBand = 1.0 - smoothstep(kSlopeRockLo, kSlopeRockHi, slope);
    // Wobble the rock edge with noise so it isn't a clean contour line.
    rockBand = clamp(rockBand + n * 0.18, 0.0, 1.0);
    albedo = mix(albedo, rock, rockBand * (1.0 - snowBand * 0.55));

    // Subtle macro tint variation so large flat areas aren't a flat colour.
    float macro = tnoise(wpos.xz * (kMacroScale * 0.5));
    albedo *= mix(0.88, 1.10, macro);

    return albedo;
}

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
float rtshVisibility(vec3 origin, vec3 dir, float tMax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, rtShadowTlas,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
        0xFF, origin, 0.01, dir, tMax);
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

// ===========================================================================
// DDGI probe-field sample (r_ddgi) — classic Majercik et al. 2019 weighting.
// Trilinear over the 8 surrounding probes, each weight shaped by:
//   * a soft BACKFACE term (probes behind the surface contribute little),
//   * the CHEBYSHEV visibility test against the probe's mean/mean^2 depth
//     (statistical occlusion — the no-leak part: a probe across a wall sees
//     a much shorter mean distance in this direction than the fragment's
//     actual distance, so its weight collapses),
//   * a self-shadow BIAS (the sample point is nudged along normal + view so
//     surface-adjacent rays don't read their own wall).
// Returns rgb = DDGI irradiance (same E units as irradianceCube), a = grid
// CONFIDENCE in [0,1] (1 well inside the probe volume, fading to 0 at/outside
// its boundary — the caller lerps from the IBL/flat ambient by this).
// ===========================================================================
vec2 ddgiSignNotZero(vec2 v) { return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 ddgiOctEncode(vec3 v) {
    v /= (abs(v.x) + abs(v.y) + abs(v.z));
    vec2 e = v.xy;
    if (v.z < 0.0) e = (1.0 - abs(e.yx)) * ddgiSignNotZero(e);
    return e;                                       // [-1,1]^2
}
// Probe tile UV inside an atlas: tileU = px + py*countX, tileV = pz; T texels
// interior + 1px border each side. Matches ddgi_update.comp's layout exactly.
vec2 ddgiAtlasUV(ivec3 p, vec3 dir, float T) {
    float tile = T + 2.0;
    vec3 counts = ssao.ddgiCounts.xyz;
    vec2 tileBase = vec2(float(p.x) + float(p.y) * counts.x, float(p.z)) * tile;
    vec2 oct = ddgiOctEncode(dir) * 0.5 + 0.5;
    vec2 texel = tileBase + 1.0 + oct * T;
    vec2 atlasSize = vec2(counts.x * counts.y * tile, counts.z * tile);
    return texel / atlasSize;
}
vec4 sampleDdgi(vec3 P, vec3 N, vec3 V) {
    vec3 origin  = ssao.ddgiOrigin.xyz;
    vec3 spacing = max(ssao.ddgiSpacing.xyz, vec3(1e-3));
    vec3 counts  = ssao.ddgiCounts.xyz;
    float visMaxDist = ssao.ddgiOrigin.w;

    // Self-shadow bias: nudge the sample point off the surface toward the
    // viewer so probe rays that stopped ON this wall don't occlude it.
    float minSpacing = min(spacing.x, min(spacing.y, spacing.z));
    vec3  biasVec = (N * 0.6 + V * 0.4) * (minSpacing * 0.25 * ssao.ddgiCtrl.w);
    vec3  Pb = P + biasVec;

    vec3 g = (Pb - origin) / spacing;
    vec3 baseF = floor(g);
    vec3 alpha = clamp(g - baseF, 0.0, 1.0);
    ivec3 base = ivec3(baseF);
    ivec3 maxC = ivec3(counts) - 1;

    // Grid confidence: 1 inside, fades to 0 over the outermost half-cell.
    vec3 gc = (P - origin) / spacing;
    vec3 edge = min(gc, counts - 1.0 - gc);        // cells to the nearest face
    float contain = clamp(min(edge.x, min(edge.y, edge.z)) * 2.0 + 1.0, 0.0, 1.0);
    if (contain <= 0.0) return vec4(0.0);

    vec3 sumIrr = vec3(0.0);
    float sumW = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 pc = clamp(base + o, ivec3(0), maxC);
        vec3 probePos = origin + vec3(pc) * spacing;

        vec3 toProbe = probePos - Pb;
        float distToProbe = max(length(toProbe), 1e-4);
        vec3 dirToProbe = toProbe / distToProbe;

        // Trilinear weight (from the UNclamped cell alpha).
        vec3 tri = mix(1.0 - alpha, alpha, vec3(o));
        float w = max(tri.x * tri.y * tri.z, 1e-5);

        // Soft backface (Majercik): smooth, never fully zero.
        float wn = (dot(dirToProbe, N) + 1.0) * 0.5;
        w *= wn * wn + 0.2;

        // Chebyshev visibility from the probe's depth moments along the
        // probe->point direction.
        vec2 mm = texture(ddgiVisTex, ddgiAtlasUV(pc, -dirToProbe, 14.0)).rg;
        float mean = mm.x;
        float r = min(distToProbe, visMaxDist);
        if (r > mean) {
            float variance = abs(mm.y - mm.x * mm.x) + 1e-4;
            float d = r - mean;
            float cheb = variance / (variance + d * d);
            w *= max(cheb * cheb * cheb, 0.0);
        }
        w = max(w, 1e-6);

        sumIrr += texture(ddgiIrrTex, ddgiAtlasUV(pc, N, 6.0)).rgb * w;
        sumW += w;
    }
    return vec4(sumIrr / max(sumW, 1e-5), contain);
}

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
        vec4 rr  = texture(reflTex, ruv);
        float rw = clamp(rr.a, 0.0, 1.0) * clamp(ssao.refl.y, 0.0, 1.0)
                 * (1.0 - smoothstep(0.25, 0.6, perceptualRough));
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
// ============================================================================
// LIGHT-UNIT CONVENTION (engine-wide; the fix for "GLB meshes are unlit").
//
// This engine has TWO direct-lighting paths in this shader, chosen per object by
// "does it carry an MR map": the DIELECTRIC path (no MR — every procedural prim:
// floors, walls, level geometry) and this PBR path (any MR map — which is EVERY
// GLB, because the model loader synthesizes a 1x1 MR from the glTF factors).
//
// The dielectric path evaluates a plain, UNNORMALIZED Lambert: albedo * N.L * C.
// It does NOT divide by PI. Every light rig in the game (point intensities,
// ranges, sun color) was authored and eyeballed against THAT convention — i.e.
// the light's `color * intensity` is authored as a PI-scaled irradiance, not as
// radiance. The PBR path used the textbook normalized Lambert (albedo/PI), so
// under the SAME light a GLB shaded 1/PI = 0.318x of the prim standing next to
// it (metallic 0), and ~0.03x once metallic climbed to 0.8 — the "GLB meshes are
// effectively unlit" bug. Measured: a white probe cube at one world position,
// prim vs GLB, is BYTE-IDENTICAL on the same path (the loader/normals are
// correct) and drops by exactly this factor when it crosses to the PBR path.
//
// So the PBR path adopts the SAME convention as the dielectric path: no 1/PI,
// and the caller passes the same diffuse weight the dielectric path uses for
// that light (sun 0.75, point lights 1.0). Energy still conserves — (1-F) hands
// the Fresnel share to the specular lobe, which the dielectric path simply
// throws away. Result: a metallic=0 GLB and a prim with the same albedo now
// shade within ~4% of each other under identical lights, and metals get a real
// (rather than PI-crushed) diffuse+spec response.
// ============================================================================
const float kSunDiffuseW   = 0.75;   // matches the dielectric path's sun weight
const float kPointDiffuseW = 1.0;    // matches the dielectric path's point-light weight

// One light's outgoing radiance factor (Lambert diffuse + GGX spec) * NoL.
// `dw` = the path-parity diffuse weight above (NOT 1/PI — see the convention note).
vec3 brdf(vec3 N, vec3 V, float NoV, vec3 L, vec3 F0, vec3 diff, float a, float dw) {
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
    vec3 F = F_Schlick(VoH, F0);
    vec3 spec = D_GGX(NoH, a) * V_SmithGGX(NoV, NoL, a) * F;
    return ((vec3(1.0) - F) * diff * dw + spec) * NoL;
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
    // TWO-SIDED lighting (pipeline is cull NONE): flip the normal on backfaces so
    // mixed-winding HDRP kit meshes shade correctly from both sides — without this
    // a flipped sub-mesh lights as if facing away (black wall).
    if (!gl_FrontFacing) N = -N;
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
    // Per-scene SUN RADIANCE (SkyParams::sunLight, packed in cam.sunDir.w). 1.0 for
    // every world that never sets it -> byte-identical to the old hardcoded sun.
    // Deep space raises it: a star is the only light out there.
    vec3  sunRad  = kSunColor * max(cam.sunDir.w, 0.0);
    float ndl    = max(dot(N, kSunDir), 0.0);
    float shadow = sampleShadow(vWorldPos, ndl);
#ifdef RT_SHADOWS
    // RT soft shadows (r_rtshadows): per-pixel seed (per-frame rotated when TAA
    // is on — rtsh1.x is then a running frame counter; pinned 0 with TAA off so
    // the dither is static) + the per-pixel point-shadow ray budget. The
    // geometric normal (NOT the normal-mapped one) offsets ray origins so bump
    // detail can't push the origin through its own surface.
    uint rtshSeed = rtshWang(uint(gl_FragCoord.x) * 1973u
                           + uint(gl_FragCoord.y) * 9277u
                           + uint(ssao.rtsh1.x)   * 26699u);
    int  rtshRaysLeft = int(ssao.rtsh0.z);
    vec3 rtshNg = normalize(vNormal);
    // TWO-SIDED (see the N flip above): mixed-winding pack meshes present their
    // BACK face — the unflipped vertex normal then points INTO the solid, so ray
    // origins offset inside the wall and every sun ray self-intersects (whole
    // districts read shadow=0 = pitch black). Flip the ray-offset normal too.
    if (!gl_FrontFacing) rtshNg = -rtshNg;
    // SUN (tier >= 1): min() with the CSM term — the traced ray gives the
    // contact-hardening penumbra from STATIC geometry; the raster map keeps
    // shadows from skinned characters (absent from the static TLAS). Skip the
    // ray when the sun term is already dead (backface / fully CSM-shadowed).
    if (ssao.rtsh0.x >= 0.5 && ndl > 0.0 && shadow > 0.001)
        shadow = min(shadow, rtshSunVisibility(vWorldPos, rtshNg, kSunDir, rtshSeed));
#endif
    vec3  ambient = cam.ambientCount.rgb;
    float up = N.y * 0.5 + 0.5;                 // 0 = facing down, 1 = facing up
    float ao = 1.0;
    if (ssao.ctrl.x > 0.5) {
        vec2 aoUV = gl_FragCoord.xy * ssao.ctrl.zw;   // pixel -> [0,1] screen UV
        ao = mix(1.0, texture(ssaoTex, aoUV).r, clamp(ssao.ctrl.y, 0.0, 1.0));
    }
    int nLights = int(cam.ambientCount.w);

    // ---- DDGI probe-field irradiance (r_ddgi): sampled ONCE per fragment,
    // shared by both shading paths. Inactive (gate 0) -> exact zero weight and
    // the atlases are never sampled — the prior ambient math is unchanged. ----
    vec4 ddgiGI = vec4(0.0);
    if (ssao.ddgiCtrl.x > 0.5) {
        vec3 Vg = normalize(cam.camPos.xyz - vWorldPos);
        ddgiGI = sampleDdgi(vWorldPos, N, Vg);
        ddgiGI.rgb *= ssao.ddgiCtrl.y;            // intensity (warm-up ramped)
        ddgiGI.a   *= clamp(ssao.ddgiCtrl.y, 0.0, 1.0);
    }

    // ---- r_debugview 1: SHADING NORMALS. The one-frame answer to "is this surface
    // dark because the light can't reach it, or because its normal points into the
    // wall?" (KNOWN_BUGS R3 / the graybox-wall hunt). Off by default; zero cost.
    if (ssao.rtsh1.w > 0.5 && ssao.rtsh1.w < 1.5) {
        outColor = vec4(N * 0.5 + 0.5, 1.0);
        return;
    }
    // r_debugview 2: the POINT-LIGHT DIFFUSE TERM ALONE (no albedo, no ambient, no
    // sun). Whatever is on the light path glows; whatever is not is BLACK. There is
    // nowhere for a photon to hide in this view.
    // r_debugview 3: ALBEDO ALONE (base texture x baseColor factor). The other half
    // of "value, not lumens": a surface can be perfectly on the light path and still
    // read black, because it is multiplying that light by ~nothing.
    if (ssao.rtsh1.w > 2.5 && ssao.rtsh1.w < 3.5) {
        outColor = vec4(albedo.rgb, 1.0);
        return;
    }
    // r_debugview 5: DECISIONS.md's own probe — shade the scene normally but force a
    // flat 0.5 WHITE albedo on everything. "Is the surface receiving light at all?"
    if (ssao.rtsh1.w > 4.5) albedo.rgb = vec3(0.5);
    // r_debugview 4: the AMBIENT / IBL term alone (dielectric params) — everything a
    // surface gets that did NOT come from a lamp.
    if (ssao.rtsh1.w > 3.5 && ssao.rtsh1.w < 4.5) {
        vec3 Vq = normalize(cam.camPos.xyz - vWorldPos);
        outColor = vec4(iblAmbient(N, Vq, albedo.rgb, 0.0, 0.5, vec3(0.04), ao,
                                   cam.ambientCount.rgb, N.y * 0.5 + 0.5, vec4(0.0)), 1.0);
        return;
    }
    if (ssao.rtsh1.w > 1.5 && ssao.rtsh1.w < 2.5) {
        vec3 dbg = vec3(0.0);
        int  nl  = int(cam.ambientCount.w);
        for (int i = 0; i < nl && i < kMaxPointLights; ++i) {
            vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
            float dist = length(toL);
            vec3  L    = toL / max(dist, 0.0001);
            dbg += cam.lights[i].colorPad.rgb
                 * (max(dot(N, L), 0.0) * pointAtten(dist, cam.lights[i].posRange.w));
        }
        outColor = vec4(dbg, 1.0);
        return;
    }

    vec3 color;
    if (vMrTexIndex == 0u) {
        // ---- DIELECTRIC path (no MR map: every graybox PRIM). ------------------------
        // ONE LIGHTING PATH (KNOWN_BUGS R1, finished 2026-07-12). R1 unified the DIFFUSE
        // between the two branches and stopped there, so this branch still had NO DIRECT
        // SPECULAR AT ALL while every GLB beside it got a full GGX lobe. Measured on the
        // --test-primlight rig (identical albedo, identical geometry, identical lamp):
        // a 12.5% radiance split, invisible for a year because the 0.42 ambient wash and
        // a full-strength sky IBL were filling the gap. It is now literally the SAME
        // brdf() the PBR branch calls, with the dielectric's own constants (metallic = 0,
        // F0 = 0.04, satin roughness 0.5) — not a copy of the maths, the maths.
        vec3  Vd = normalize(cam.camPos.xyz - vWorldPos);
        const float kDielectricRough = 0.5;   // satin clad/floor default
        const float aD  = kDielectricRough * kDielectricRough;   // -> GGX alpha
        vec3  F0d = vec3(0.04);
        float NoVd = max(dot(N, Vd), 1e-4);
        vec3  lit  = brdf(N, Vd, NoVd, kSunDir, F0d, albedo.rgb, aD, kSunDiffuseW) * sunRad * shadow;
        for (int i = 0; i < nLights && i < kMaxPointLights; ++i) {
            vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
            float dist = length(toL);
            vec3  L    = toL / max(dist, 0.0001);
            float atten = pointAtten(dist, cam.lights[i].posRange.w);
#ifdef RT_SHADOWS
            // POINT RT shadow (tier >= 2): the first K lights with a real
            // contribution here each get one source-jittered shadow ray;
            // negligible / over-budget lights keep the unshadowed behavior.
            float vis = 1.0;
            if (ssao.rtsh0.x >= 1.5 && rtshRaysLeft > 0 && dot(N, L) > 0.0
                && atten * dot(cam.lights[i].colorPad.rgb, vec3(0.299, 0.587, 0.114)) > 0.004) {
                --rtshRaysLeft;
                vis = rtshPointVisibility(vWorldPos, rtshNg, toL, dist, rtshSeed);
            }
            // r_debugview 7: point RT shadows forced lit — the A/B that caught
            // the glass-in-TLAS self-occlusion (2026-07-30). Kept: zero cost,
            // and it splits "light missing" from "light shadowed" instantly.
            if (ssao.rtsh1.w > 6.5) vis = 1.0;
            atten *= vis;
#endif
            lit += brdf(N, Vd, NoVd, L, F0d, albedo.rgb, aD, kPointDiffuseW)
                 * cam.lights[i].colorPad.rgb * atten;
        }
        color = lit
              + iblAmbient(N, Vd, albedo.rgb, 0.0, kDielectricRough, F0d, ao, ambient, up, ddgiGI);
    } else {
        // ---- PBR metallic-roughness (Cook-Torrance GGX). glTF MR: B=metallic, G=roughness. ----
        vec3  mr       = texture(textures[nonuniformEXT(vMrTexIndex)], vUV).rgb;
        // BLACK-PROP FIX (room-dressing kit props). Dark-albedo kit furniture (crates,
        // beds, vats, chairs) authors metallic=1 in its MR map. A full metal has NO
        // diffuse lobe (diff = albedo*(1-metallic) -> 0) and its only ambient response
        // is the F0-tinted environment reflection — which, in a windowless facility with
        // a dark baked env, is ~black. Result: the props render as flat black silhouettes
        // even under bright ceiling/pendant lights, while the diffuse graybox shell reads
        // fine. The dressing passes a per-object metallic CLAMP in vGlassParams.w (the
        // otherwise-unused glass lane on opaque draws) so these props keep a diffuse lobe
        // that actually catches the room + flashlight lighting in BOTH gameplay and the
        // capture rig. 0 = untouched (every non-dressing draw -> byte-identical shading).
        float mrClamp  = vGlassParams.w > 0.0 ? vGlassParams.w : 1.0;
        float metallic = mr.b * mrClamp;
        float pRough   = clamp(mr.g - detSmoothAdj * 0.4, 0.045, 1.0);  // perceptual roughness (for IBL) + detail-smoothness nudge
        float a        = pRough; a *= a;                         // -> GGX alpha (direct lights)
        vec3  F0       = mix(vec3(0.04), albedo.rgb, metallic);
        vec3  diff     = albedo.rgb * (1.0 - metallic);
        vec3  V        = normalize(cam.camPos.xyz - vWorldPos);
        float NoV      = max(dot(N, V), 1e-4);
        vec3  Lo = brdf(N, V, NoV, kSunDir, F0, diff, a, kSunDiffuseW) * sunRad * shadow;  // sun (shadowed)
        for (int i = 0; i < nLights && i < kMaxPointLights; ++i) {                    // point lights
            vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
            float dist = length(toL);
            vec3  L    = toL / max(dist, 0.0001);
#ifdef RT_SHADOWS
            // POINT RT shadow (tier >= 2): same first-K-significant policy as
            // the dielectric loop (one budget shared across both paths).
            float atten = pointAtten(dist, cam.lights[i].posRange.w);
            float vis = 1.0;
            if (ssao.rtsh0.x >= 1.5 && rtshRaysLeft > 0 && dot(N, L) > 0.0
                && atten * dot(cam.lights[i].colorPad.rgb, vec3(0.299, 0.587, 0.114)) > 0.004) {
                --rtshRaysLeft;
                vis = rtshPointVisibility(vWorldPos, rtshNg, toL, dist, rtshSeed);
            }
            Lo += brdf(N, V, NoV, L, F0, diff, a, kPointDiffuseW) * cam.lights[i].colorPad.rgb * (atten * vis);
#else
            Lo += brdf(N, V, NoV, L, F0, diff, a, kPointDiffuseW) * cam.lights[i].colorPad.rgb * pointAtten(dist, cam.lights[i].posRange.w);
#endif
        }
        // Image-based lighting: SPLIT-SUM diffuse irradiance + GGX-prefiltered specular
        // from the analytic-sky environment cube, so metals reflect the sky and
        // dielectric floors/glass get real specular env. Replaces the old flat
        // ambient diffuse + ambient*3.4*Fresnel constant (kept as the no-env fallback).
        Lo += iblAmbient(N, V, albedo.rgb, metallic, pRough, F0, ao, ambient, up, ddgiGI);
        color = Lo;
    }

    // ======================================================================
    // FOLIAGE (FLAG_FOLIAGE): trees were flat black on the away-side. Two cheap
    // canopy terms: (1) WRAP — lift the diffuse so light bends around the rounded
    // crown instead of a hard terminator; (2) BACK-TRANSLUCENCY — when the camera
    // looks toward the sun THROUGH the leaves, add a warm forward-scatter glow
    // (albedo-tinted), gated by the sun shadow so a shadowed tree doesn't glow.
    // ======================================================================
    if ((vFlags & FLAG_FOLIAGE) != 0u) {
        float wrap  = clamp((dot(N, kSunDir) + 0.6) / 1.6, 0.0, 1.0);
        vec3  Vf    = normalize(cam.camPos.xyz - vWorldPos);
        float back  = pow(max(dot(Vf, -kSunDir), 0.0), 3.0);
        color += albedo.rgb * sunRad * (wrap * 0.35) * mix(0.55, 1.0, shadow)   // wrap fill
               + albedo.rgb * sunRad * (back * 0.85) * shadow;                  // sun through canopy
    }

    // ======================================================================
    // CLEARCOAT lobe (car paint, FLAG_CLEARCOAT): a SECOND specular layer with
    // a fixed dielectric F0 (0.04 — lacquer over the base coat) and its own low
    // roughness, energy-conserving: the coat's view-angle fresnel ATTENUATES the
    // base result (light reflected by the coat never reaches the paint below),
    // then the coat's own direct GGX + mirror-sharp environment (prefiltered env
    // with the SSR/RT reflection replace — the emissive-panel sweep money shot)
    // are added on top. The coat shades on the GEOMETRIC normal (a lacquer film
    // is smooth — it must not inherit micro normal-map detail), which also keeps
    // the fresnel rim clean across body curvature.
    // ======================================================================
    if ((vFlags & FLAG_CLEARCOAT) != 0u) {
        float ccI = float(vTerrainPack.x & 0xFFu) / 255.0;
        float ccR = max(float((vTerrainPack.x >> 8) & 0xFFu) / 255.0, 0.02);
        vec3  Nc  = normalize(vNormal);
        vec3  Vc  = normalize(cam.camPos.xyz - vWorldPos);
        float NoVc = max(dot(Nc, Vc), 1e-4);
        float aCc  = ccR * ccR;
        // Coat fresnel at the view angle = the energy the coat takes from the base.
        float Fc = (0.04 + 0.96 * pow(1.0 - NoVc, 5.0)) * ccI;
        color *= (1.0 - Fc);
        // Direct lights through the coat lobe (sun shadowed like the base).
        vec3 ccLo = vec3(0.0);
        {
            float NoL = max(dot(Nc, kSunDir), 0.0);
            if (NoL > 0.0) {
                vec3 H = normalize(Vc + kSunDir);
                float NoH = max(dot(Nc, H), 0.0), VoH = max(dot(Vc, H), 0.0);
                float Fd  = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
                ccLo += sunRad * shadow
                      * (D_GGX(NoH, aCc) * V_SmithGGX(NoVc, NoL, aCc) * Fd * NoL);
            }
            for (int i = 0; i < nLights && i < kMaxPointLights; ++i) {
                vec3  toL  = cam.lights[i].posRange.xyz - vWorldPos;
                float dist = length(toL);
                vec3  L    = toL / max(dist, 0.0001);
                float NoL2 = max(dot(Nc, L), 0.0);
                if (NoL2 <= 0.0) continue;
                vec3 H = normalize(Vc + L);
                float NoH = max(dot(Nc, H), 0.0), VoH = max(dot(Vc, H), 0.0);
                float Fd  = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
                ccLo += cam.lights[i].colorPad.rgb * pointAtten(dist, cam.lights[i].posRange.w)
                      * (D_GGX(NoH, aCc) * V_SmithGGX(NoVc, NoL2, aCc) * Fd * NoL2);
            }
        }
        // Coat environment: prefiltered env along the coat reflection, with the
        // SSR/RT reflection buffer REPLACING it where confident (same blend +
        // roughness gate + Reinhard rolloff as the base IBL specular).
        vec3 envC;
        if (ssao.ibl.x > 0.5) {
            vec3 Rc  = reflect(-Vc, Nc);
            vec3 pre = textureLod(prefilterCube, Rc, ccR * max(ssao.ibl.z, 0.0)).rgb;
            if (ssao.refl.x > 0.5) {
                vec2 ruv = gl_FragCoord.xy * ssao.ctrl.zw;
                vec4 rr  = texture(reflTex, ruv);
                float rw = clamp(rr.a, 0.0, 1.0) * clamp(ssao.refl.y, 0.0, 1.0)
                         * (1.0 - smoothstep(0.25, 0.6, ccR));
                pre = mix(pre, rr.rgb, rw);
            }
            vec2 ab = texture(brdfLUT, vec2(NoVc, ccR)).rg;
            envC = pre * (vec3(0.04) * ab.x + ab.y);
            envC = envC / (1.0 + envC);
            envC *= ssao.ibl.y;
        } else {
            // No baked env: the legacy flat-ambient fresnel rim, so the coat still
            // reads on paths without IBL (matches the old ambient-specular look).
            envC = ambient * 3.4 * (0.04 + 0.96 * pow(1.0 - NoVc, 5.0)) * mix(0.55, 1.1, up);
        }
        float specAoC = clamp(ao + 0.4, 0.0, 1.0);
        color += (ccLo + envC * specAoC) * ccI;
    }

    // ---- DDGI debug views (r_ddgi_debug): 1 = the raw interpolated probe
    // irradiance field, 2 = the grid confidence weight. Replaces the shaded
    // color outright (a diagnostic, gated off in normal play). ----
    if (ssao.ddgiCtrl.z > 0.5) {
        color = (ssao.ddgiCtrl.z < 1.5) ? ddgiGI.rgb : vec3(ddgiGI.a);
    }

    // ---- Emissive: per-object HDR radiance on top (glows even in shadow; feeds bloom). ----
    // ======================================================================
    // SHIP SELF-LIGHT (FLAG_SHIP_SELFLIT) — the canon "ships are self-lit" term.
    // NOT an ambient/emissive floor (a floor is what washed the capital ship to a
    // flat white slab): this is SHAPED, and it is GATED OFF WHERE THE STAR HITS.
    //   * (1 - N.L * shadow) -> the term exists ONLY on the side the sun is not
    //     lighting and fades to zero as a surface turns into the light, so the lit
    //     side keeps its honest N.L / GGX response, untouched.
    //   * a Fresnel rim (grazing edges) keeps the SILHOUETTE alive at any angle —
    //     the "looks great on the first pass, goes black on the second" case.
    //   * an N.V form term lifts the facing planes just off the floor, still
    //     varying with the normal, so the hull reads as a shaped volume, not a
    //     flat cutout.
    // Cool hull-lighting tint, capped well under 1.0 so it can never clip or bloom.
    // ======================================================================
    if ((vFlags & FLAG_SHIP_SELFLIT) != 0u) {
        const vec3 kShipSelfLight = vec3(0.34, 0.40, 0.52);   // cool steel interior light
        float sl    = float(vTerrainPack.y & 0xFFu) / 255.0;
        vec3  Vs    = normalize(cam.camPos.xyz - vWorldPos);
        float NoVs  = clamp(dot(N, Vs), 0.0, 1.0);
        float rim   = pow(1.0 - NoVs, 3.0);                   // silhouette edges
        float form  = 0.30 + 0.70 * NoVs;                     // facing planes (normal-shaped)
        // SQUARED darkness gate: the term dies FAST as soon as any starlight lands
        // on a surface, so it can only ever fill the genuinely unlit side. (Linear
        // fell off too slowly and washed the half-lit fins pale — the very failure
        // mode we removed from the old ambient floor.)
        float dark  = 1.0 - clamp(ndl * shadow, 0.0, 1.0);
        dark *= dark;
        color += kShipSelfLight * (sl * dark * (form * 0.50 + rim * 0.50));
    }

    vec3 emis = vEmissive.rgb;
    if (vEmissiveTexIndex > 0u) emis *= texture(textures[nonuniformEXT(vEmissiveTexIndex)], vUV).rgb;  // emissive map gates WHERE it glows (edge strips)
    color += emis * vEmissive.a;
    // BLEND (glass): Unity glass mats often have baseColorFactor.a=0 -> invisible under straight
    // alpha-over. Floor the opacity + add a fresnel grazing term so glass reads as a translucent,
    // reflective pane. Gated on vFactor.a<0.99 so a=1 BLEND overlays (screens) stay solid.
    float outA = albedo.a;
    if (alphaBlend && vFactor.a < 0.99) {
        vec3  Vv     = normalize(cam.camPos.xyz - vWorldPos);
        float fres   = pow(1.0 - max(dot(N, Vv), 0.0), 5.0);
        if (vFactor.a > 0.0 && vFactor.a < 0.07) {
            // NEAR-CLEAR canopy glass: an authored tiny alpha is intentional —
            // honor it literally so a starfield punches through (the 0.10 floor
            // below washed deep-space canopies into grey fog). Kept out of the
            // a==0 case, which stays the Unity-broken-material rescue.
            outA = clamp(vFactor.a + fres * 0.12, 0.0, 1.0);          // faint fresnel edge shine
            color += kSunColor * fres * 0.03;                        // whisper of grazing rim
        } else {
            float baseOp = mix(0.10, 0.32, clamp(vFactor.a, 0.0, 1.0));   // mostly SEE-THROUGH (was washing white)
            outA = clamp(baseOp + fres * 0.35, 0.0, 1.0);                 // edges firmer, face near-clear
            color += kSunColor * fres * 0.06;                            // subtle grazing rim only
        }
    }
    outColor = vec4(color, outA);   // HDR linear; tonemap is in composite.frag
}
