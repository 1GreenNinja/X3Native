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
    // ---- CLUSTERED FORWARD LIGHTING (r_clusterlights; see inc/mesh_lighting.glsl)
    // APPENDED at the tail, so mesh.vert (which declares the block only as far as
    // camPos) and glass.frag stay valid std140 PREFIXES of this same buffer.
    // All four are ZERO when r_clusterlights is 0, and clusterCfg.x == 0 is what
    // routes every light loop back to the legacy 64-entry UBO array above.
    vec4 camFwd;                    // xyz = camera FORWARD (view basis), w = zNear
    vec4 clusterCfg;                // x = clustered active (0/1), y = scene light count, zw = 1/screenW, 1/screenH
    vec4 clusterGrid;               // x = grid X, y = grid Y, z = grid Z, w = max lights per froxel
    vec4 clusterSlice;              // x = sliceScale, y = sliceBias, z = froxel count, w = reserved
} cam;

// Hardware-compare shadow sampler (depth texture + VK_COMPARE_OP_LESS_OR_EQUAL).
// texture(...) returns the PCF-filtered "fragment is lit" fraction in [0,1].
// CSM (Lane 3): a 2D ARRAY, one layer per cascade. Layer 0 IS the legacy
// cascade, so with r_csm 0 this samples exactly the texels the single-map
// renderer wrote. Consumed by inc/mesh_shadows.glsl.
layout(set = 2, binding = 0) uniform sampler2DArrayShadow shadowMap;

// CASCADED SHADOW MAPS control block. Mirrors CsmUBO in
// engine/rhi/vk/VulkanRenderDevice_internal.h EXACTLY - keep them in sync.
// ctrl.x == 0 means "no cascades this frame" -> sampleShadow takes its legacy
// branch. glass.frag declares this identically (it shares the descriptor set).
layout(set = 2, binding = 1) uniform Csm {
    mat4 viewProj[4];   // world -> cascade i's shadow clip
    vec4 splitFar;      // lane i = cascade i's far VIEW depth (meters)
    vec4 depthBias;     // lane i = constant bias, light-clip depth units
    vec4 normalBias;    // lane i = world-space normal offset (meters)
    vec4 ctrl;          // x = active cascade count (0 = legacy), y = blend-band fraction
} csm;

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
    vec4 refl;        // x=reflections active (0/1), y=intensity, z=env-specular scale (r_iblspec), w=glossy disc scale when the DENOISE stage ran (r_refldenoise; 0 = stage off -> legacy scale 1.0)
    // ---- DDGI probe-grid irradiance (r_ddgi; ray-query hardware only) ----
    vec4 ddgiCtrl;    // x=active (0/1), y=intensity (warm-up ramped), z=debug mode (0/1/2), w=self-shadow bias scale
    vec4 ddgiOrigin;  // xyz = probe-grid min corner (world), w = visMaxDist (m)
    vec4 ddgiSpacing; // xyz = probe spacing (m), w = unused
    vec4 ddgiCounts;  // xyz = probe counts (as float), w = unused
    // ---- RT soft shadows (r_rtshadows; read ONLY by the RT_SHADOWS variant) ----
    vec4 rtsh0;       // x = tier (0=off,1=sun,2=sun+points), y = tan(sun angular radius), z = max point shadow rays K, w = point light source radius (m)
    vec4 rtsh1;       // x = frame seed (per-frame jitter rotation; 0 when TAA is off), yzw = reserved
    // ---- Underwater caustics (setCaustics; all zero when no host opted in) ----
    vec4 caustics;    // x = enabled (0/1), y = local water surface Y, z = time (s), w = intensity
    // ---- TERRAIN NORMAL MAPS (registerTerrainMaterial's normal handles) ----
    // Packed exactly like the per-object terrain ALBEDO pack so the two read the
    // same way in inc/mesh_terrain.glsl: x = grass<<16|rock, y = snow<<16|sand.
    // These live here rather than in the object SSBO row because the terrain
    // material set is DEVICE-GLOBAL — one set per device, identical on every
    // terrain draw — and there is no free lane in the SSBO row that terrain does
    // not already spend. APPENDED at the tail so glass.frag, which declares this
    // same buffer only as far as `caustics`, stays a valid std140 prefix.
    // All zero (the default) means "no normal maps" -> terrain shades from the
    // geometry normal, byte-identical to the pre-relief renderer.
    uvec4 terrainNrm; // xy = the two packs (see above), zw = reserved
    // ---- Surface wetness (setWetness; amount 0 = gate shut, byte-identical) ----
    vec4 wetness;     // x = amount (0..1 soak), y = porosity, z = puddles, w = min roughness
} ssao;
// Screen-traced / ray-traced reflection buffer (set3/binding2, half- or full-res
// RGBA16F): rgb = reflected radiance from the REFLECTION pass (refl.comp — SSR
// march against the depth buffer sampling LAST frame's lit scene, with an
// optional ray-query fallback), a = confidence [0,1]. Sampled at the fragment's
// screen UV (the ssaoTex pattern) and blended into the IBL specular below,
// gated by ssao.refl.x — when 0 this texture is never read and the IBL path is
// byte-for-byte the pre-reflections math.
layout(set = 3, binding = 2) uniform sampler2D reflTex;
// DENOISED reflection buffer (set3/binding6, r_refldenoise): the output of the
// edge-aware a-trous chain (shaders/refl_denoise.comp) over that same buffer.
//
// WHEN THE DENOISE STAGE IS OFF this descriptor is bound to the VERY SAME image
// view as reflTex and ssao.refl.w is 0, so every expression below reduces to the
// pre-denoise arithmetic on the pre-denoise texels — BIT-EXACT. That is the
// r_refldenoise 0 contract that keeps the md5 gates holding, and it needs no
// branch: same texels, and a disc-radius scale of exactly 1.0.
//
// WHY TWO BINDINGS INSTEAD OF ONE DENOISED BUFFER: a MIRROR needs no denoise.
// Material roughness is NOT available in the reflection buffer (see the note in
// engine/rhi/ReflDenoise.h: this is a forward renderer with no G-buffer, and
// refl.comp is a depth-only pass with no material binding at all), so the
// CONSUMER — which does know per-fragment roughness — picks the buffer instead.
// The rough <= 0.05 mirror early-out below reads the RAW buffer and is therefore
// completely untouched by this lane; the glossy disc, which is the lobe the
// measured mottling actually lives on, reads the denoised one.
layout(set = 3, binding = 6) uniform sampler2D reflDnTex;

#include "inc/mesh_reflections.glsl"   // sampleReflGlossy/Denoised/Auto: roughness-aware reflection tap  [LANE 1 + refl-denoise]
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

#include "inc/mesh_terrain.glsl"   // procedural height/slope terrain splat

#include "inc/mesh_shadows.glsl"   // RT shadow rays + 3x3 PCF sampleShadow             [LANE 3]

#include "inc/mesh_lighting.glsl"   // point-light attenuation + the light loops         [LANE 2]

#include "inc/mesh_material.glsl"   // shared GGX / Smith / Schlick primitives

#include "inc/mesh_ddgi.glsl"   // DDGI probe-field irradiance sampling

#include "inc/mesh_ibl.glsl"   // split-sum IBL ambient + the reflection blend      [LANE 1]
#include "inc/mesh_brdf.glsl"   // shared direct-light BRDF + diffuse weights
#include "inc/mesh_caustics.glsl"   // underwater caustics modulation

#include "inc/mesh_normalmap.glsl"   // perturbNormal: derivative-TBN normal mapping
#include "inc/mesh_wetness.glsl"   // rain wetness: darken/smooth/F0 + exposure & pooling  [LANE: wetness]

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
        // ORDER MATTERS: the splat picks its layers from the SLOPE, so both the
        // albedo and the relief must be keyed off the GEOMETRY normal. Perturb N
        // only after the albedo has been chosen, or a rock face's own bump would
        // start voting on whether it is a rock face.
        albedo = vec4(terrainAlbedo(vWorldPos, N, vTerrainPack), 1.0) * vFactor;
        N = terrainNormal(vWorldPos, N, ssao.terrainNrm.xy);
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
    // UNDERWATER CAUSTICS: modulate the direct sun on submerged fragments (the
    // riverbed, the fish, the swimmer). Uniform-flag gate — dry worlds never
    // enter; enabled worlds return exactly 1.0 for fragments above the water.
    if (ssao.caustics.x > 0.5)
        sunRad *= causticMod(vWorldPos);
    float ndl    = max(dot(N, kSunDir), 0.0);
    float shadow = sampleShadow(vWorldPos, N, ndl);
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
    // How many lights this FRAGMENT must evaluate. Legacy (r_clusterlights 0):
    // min(activeCount, 64) — the whole UBO array, exactly as before. Clustered:
    // only the lights assigned to this fragment's froxel. Resolved once here and
    // reused by every loop below (the froxel lookup is not repeated per loop).
    int nLights = x3LightCount();

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
    // r_debugview 8: the blurred SSAO buffer, raw. The instrument that separates
    // "the AO term is wrong" from "the shading on top of it is wrong" — and the
    // one that found the half-res/full-res depth-seam pinstripe (see ssao.frag).
    if (ssao.rtsh1.w > 7.5 && ssao.rtsh1.w < 8.5) {
        outColor = vec4(vec3(texture(ssaoTex, gl_FragCoord.xy * ssao.ctrl.zw).r), 1.0);
        return;
    }
    // r_debugview 6: the CLUSTER OCCUPANCY heatmap — how many lights this
    // fragment's froxel actually holds. Black = none, blue->green->red as the
    // list fills, WHITE where the froxel is at the cap and lights are being
    // dropped. Points straight at any overflow, in one frame, by eye.
    if (ssao.rtsh1.w > 5.5 && ssao.rtsh1.w < 6.5) {
        outColor = vec4(x3ClusterHeatmap(), 1.0);
        return;
    }
    if (ssao.rtsh1.w > 1.5 && ssao.rtsh1.w < 2.5) {
        vec3 dbg = vec3(0.0);
        for (int i = 0; i < nLights; ++i) {
            ClusterLight PL = x3Light(i);
            vec3  toL  = PL.posRange.xyz - vWorldPos;
            float dist = length(toL);
            vec3  L    = toL / max(dist, 0.0001);
            dbg += PL.colorPad.rgb
                 * (max(dot(N, L), 0.0) * pointAtten(dist, PL.posRange.w));
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
        const float aDdry = kDielectricRough * kDielectricRough;  // folded at compile time
        vec3  F0d = vec3(0.04);
        float dRough = kDielectricRough;
        float aD = aDdry;
        // RAIN WETNESS on the dielectric path too: a graybox street has to get
        // wet exactly like a textured one, or the world rains in patches. Applied
        // BEFORE aD derives from the roughness. metallic = 0 by definition here.
        // THIS BRANCH IS THE UNTEXTURED PATH — terrain splats and graybox, i.e.
        // ground, rock and grass. Those are PERVIOUS: rain soaks in, so they
        // darken but must not gloss. Sending them down the impervious path put
        // bright specular streaking across the green hillsides at noon (the
        // FLAG_TERRAIN test alone missed it: this world's terrain reaches here
        // WITHOUT that flag set, which is why gating on the flag changed
        // nothing at all in the A/B).
        // GATED so the DRY path keeps its original codegen exactly. Making
        // kDielectricRough mutable to pass it by reference cost the compiler its
        // 0.5*0.5 constant fold, and that alone shifted 119 pixels by one LSB
        // with wetness OFF — small, but "off is byte-identical" is a claim this
        // project actually checks, so it has to be true.
        if (ssao.wetness.x > 0.0) {
            applyWetness(albedo.rgb, dRough, F0d, N, ao, 0.0, ssao.wetness,
                         kWetTerrainGloss);
            aD = dRough * dRough;
        }
        float NoVd = max(dot(N, Vd), 1e-4);
        vec3  lit  = brdf(N, Vd, NoVd, kSunDir, F0d, albedo.rgb, aD, kSunDiffuseW) * sunRad * shadow;
        for (int i = 0; i < nLights; ++i) {
            ClusterLight PL = x3Light(i);
            vec3  toL  = PL.posRange.xyz - vWorldPos;
            float dist = length(toL);
            vec3  L    = toL / max(dist, 0.0001);
            float atten = pointAtten(dist, PL.posRange.w);
#ifdef RT_SHADOWS
            // POINT RT shadow (tier >= 2): the first K lights with a real
            // contribution here each get one source-jittered shadow ray;
            // negligible / over-budget lights keep the unshadowed behavior.
            float vis = 1.0;
            if (ssao.rtsh0.x >= 1.5 && rtshRaysLeft > 0 && dot(N, L) > 0.0
                && atten * dot(PL.colorPad.rgb, vec3(0.299, 0.587, 0.114)) > 0.004) {
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
                 * PL.colorPad.rgb * atten;
        }
        color = lit
              + iblAmbient(N, Vd, albedo.rgb, 0.0, dRough, F0d, ao, ambient, up, ddgiGI);
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
        vec3  F0       = mix(vec3(0.04), albedo.rgb, metallic);
        // RAIN WETNESS. Ordered deliberately: F0 is built from the DRY albedo
        // (so a metal keeps its own tint), then the film darkens/smooths/raises
        // what it should, and only THEN do `a` and `diff` derive from the
        // wet values. amount 0 returns on the first compare -> byte-identical.
        // gloss: terrain (soil/grass/scree) is PERVIOUS — water soaks in, so it
        // darkens without glossing; everything else is a built surface that
        // holds a film. See the note in mesh_wetness.glsl.
        if (ssao.wetness.x > 0.0)
            applyWetness(albedo.rgb, pRough, F0, N, ao, metallic, ssao.wetness,
                         (vFlags & FLAG_TERRAIN) != 0u ? kWetTerrainGloss : 1.0);
        float a        = pRough; a *= a;                         // -> GGX alpha (direct lights)
        vec3  diff     = albedo.rgb * (1.0 - metallic);
        vec3  V        = normalize(cam.camPos.xyz - vWorldPos);
        float NoV      = max(dot(N, V), 1e-4);
        vec3  Lo = brdf(N, V, NoV, kSunDir, F0, diff, a, kSunDiffuseW) * sunRad * shadow;  // sun (shadowed)
        for (int i = 0; i < nLights; ++i) {                                           // point lights
            ClusterLight PL = x3Light(i);
            vec3  toL  = PL.posRange.xyz - vWorldPos;
            float dist = length(toL);
            vec3  L    = toL / max(dist, 0.0001);
#ifdef RT_SHADOWS
            // POINT RT shadow (tier >= 2): same first-K-significant policy as
            // the dielectric loop (one budget shared across both paths).
            float atten = pointAtten(dist, PL.posRange.w);
            float vis = 1.0;
            if (ssao.rtsh0.x >= 1.5 && rtshRaysLeft > 0 && dot(N, L) > 0.0
                && atten * dot(PL.colorPad.rgb, vec3(0.299, 0.587, 0.114)) > 0.004) {
                --rtshRaysLeft;
                vis = rtshPointVisibility(vWorldPos, rtshNg, toL, dist, rtshSeed);
            }
            Lo += brdf(N, V, NoV, L, F0, diff, a, kPointDiffuseW) * PL.colorPad.rgb * (atten * vis);
#else
            Lo += brdf(N, V, NoV, L, F0, diff, a, kPointDiffuseW) * PL.colorPad.rgb * pointAtten(dist, PL.posRange.w);
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
            for (int i = 0; i < nLights; ++i) {
                ClusterLight PL = x3Light(i);
                vec3  toL  = PL.posRange.xyz - vWorldPos;
                float dist = length(toL);
                vec3  L    = toL / max(dist, 0.0001);
                float NoL2 = max(dot(Nc, L), 0.0);
                if (NoL2 <= 0.0) continue;
                vec3 H = normalize(Vc + L);
                float NoH = max(dot(Nc, H), 0.0), VoH = max(dot(Vc, H), 0.0);
                float Fd  = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
                ccLo += PL.colorPad.rgb * pointAtten(dist, PL.posRange.w)
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
                // Clearcoat lobe: same glossy treatment. A coat is usually very
                // smooth (ccR small), so this is normally the single-tap path --
                // but a satin/matte coat now blurs rather than losing the
                // reflection, which is exactly the car-paint case.
                vec4 rr  = sampleReflAuto(ruv, ccR, ssao.ctrl.zw);
                float rw = clamp(rr.a, 0.0, 1.0) * clamp(ssao.refl.y, 0.0, 1.0)
                         * (1.0 - smoothstep(0.55, 0.95, ccR));
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
