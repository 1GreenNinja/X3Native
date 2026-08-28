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
    vec4 colorPad;   // rgb = linear color * intensity, a = cos(inner half-angle)
    vec4 dirCone;    // xyz = spot axis (all-zero = OMNI),  w = cos(outer half-angle)
};
const int kMaxPointLights = 64;

layout(set = 1, binding = 1) uniform Camera {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 ambientCount;              // rgb = ambient color, w = active light count
    PointLight lights[kMaxPointLights];
    vec4 camPos;                    // xyz = camera world position (unused here; g.camPos wins)
    vec4 sunDir;                    // xyz = per-scene direction TOWARD the sun (FrameUBO tail)
    // CLUSTERED FORWARD LIGHTING tail — must mirror mesh.frag's Camera block
    // exactly (same buffer). See shaders/inc/mesh_lighting.glsl.
    vec4 camFwd;                    // xyz = camera FORWARD (view basis), w = zNear
    vec4 clusterCfg;                // x = clustered active (0/1), y = scene light count, zw = 1/screenW, 1/screenH
    vec4 clusterGrid;               // x = grid X, y = grid Y, z = grid Z, w = max lights per froxel
    vec4 clusterSlice;              // x = sliceScale, y = sliceBias, z = froxel count, w = reserved
} cam;

// CSM (Lane 3): set 2 is SHARED with mesh.frag, so these two declarations must
// match it exactly - binding 0 is a 2D shadow ARRAY (one layer per cascade,
// layer 0 = the legacy cascade), binding 1 is the per-frame CSM control block.
layout(set = 2, binding = 0) uniform sampler2DArrayShadow shadowMap;
layout(set = 2, binding = 1) uniform Csm {
    mat4 viewProj[4];
    vec4 splitFar;
    vec4 depthBias;
    vec4 normalBias;
    vec4 ctrl;          // x = active cascade count (0 = legacy), y = blend-band fraction
} csm;

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
// MEMBRANE (bit5, kFlagMembrane). An ENERGY-MEMBRANE glass surface: instead of
// the flat normal-projected bend the pane path uses, it gets a RADIAL/TANGENTIAL
// screen-space LENS plus animated TURBULENCE, so the scene behind it warps and
// LIVES. Built for the wormhole throat; any glass surface can opt in via
// GlassMaterial::lens / ::shimmer, which is how the rift-hub gates become its
// second customer. Both zero on every existing pane -> the bit is never set and
// this path is byte-identical to before.
const uint FLAG_MEMBRANE = 32u;

// ---- Membrane turbulence noise -------------------------------------------
// Value noise -> fbm, the SAME shape as the CPU-side bake in
// app/space/wormhole.cpp (integer lattice, smoothstep interpolant, octave sum on
// a NON-INTEGER lacunarity so nothing lines up on the axes). Keeping the two the
// same shape is why the animated shimmer reads as the same material the baked
// filaments do rather than as a second, unrelated texture sliding over them.
float mHash(vec2 p) {
    vec2 i = floor(p);
    return fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453123);
}

float mNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 s = f * f * (3.0 - 2.0 * f);            // == valueNoise()'s interpolant
    float a = mHash(i);
    float b = mHash(i + vec2(1.0, 0.0));
    float c = mHash(i + vec2(0.0, 1.0));
    float d = mHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}

float mFbm(vec2 p) {
    float sum = 0.0, amp = 0.5;
    for (int o = 0; o < 4; ++o) {
        sum += amp * mNoise(p);
        amp *= 0.5;
        p   *= 2.02;                              // non-integer lacunarity
    }
    return sum;
}

// Sun direction is PER-SCENE (cam.sunDir, filled from SkyParams — defaults to
// the old hardcoded (0.4,1,0.3) when no sky is set, so indoor worlds are
// unchanged). W8-2: the old const here meant a golden-hour host's LOW sun never
// matched the glass specular — the glint sat where no sun was.
const vec3 kSunColor = vec3(1.0, 0.97, 0.92);

// 3x3 PCF — the SAME sampleShadow() mesh.frag uses, now literally the same text
// (it was a hand-kept copy that had already drifted in comments only). glass.frag
// never defines RT_SHADOWS, so the ray-query block inside this include compiles
// away to nothing and the raster PCF is byte-for-byte what it always was.
#include "inc/mesh_shadows.glsl"     // RT shadow rays + 3x3 PCF sampleShadow     [LANE 3]

// Point-light attenuation + (from here on) the clustered light iteration — the
// SAME module the opaque path uses, so glass can never drift from mesh.frag's
// falloff curve or its cluster lookup again.
#include "inc/mesh_lighting.glsl"    // point-light attenuation + the light loops  [LANE 2]

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
    //
    // MASK THE ALPHA-MODE BITS FIRST. texIndex packs the alpha mode in its high
    // bits (vk_passes.cpp:970 — bit31 = MASK/cutout, bit30 = BLEND) and mesh.vert
    // forwards the word RAW; mesh.frag strips them at mesh.frag:741 before it
    // samples. This shader did not, so any glass record on the BLEND tail sampled
    // textures[0x40000000 | idx] — a wildly out-of-bounds bindless descriptor.
    // It was latent while only the additive street-light glow rode the tail; now
    // that ALL glass does (the water/shadow fix in drawMeshGlass), every pane
    // would have sampled garbage. Strip the flags exactly as mesh.frag does.
    const uint baseIdx = vTexIndex & 0x3FFFFFFFu;
    vec3 texel = texture(textures[nonuniformEXT(baseIdx)], vUV).rgb;
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

    // ---- ENERGY MEMBRANE: screen-space LENSING + heat-shimmer -------------
    // The wormhole-throat path (and any other surface that opts in). Everything
    // below returns early: a membrane is SELF-LIT, so it never runs the shadow
    // taps, the clustered light loop or the IBL reflection — which is both a
    // large saving on a heavily-overdrawn stack of annuli AND the correct look.
    // Running an energy membrane through the light loop is exactly how the
    // previous OPAQUE attempt became a flat violet plate: the wormhole's own
    // ~4200-intensity point light sits metres from the throat and blasts every
    // annulus to one uniform value, and a uniform value cannot carry structure.
    if ((vFlags & FLAG_MEMBRANE) != 0u) {
        // Params ride the SPARE terrain-pack1 lane (a membrane is never TERRAIN,
        // and clearcoat/self-light are opaque-only, so nothing collides). FOUR
        // bytes: shimmer, lens, a per-object decorrelation PHASE, and the HORIZON
        // profile selector. Byte 3 was the last free one in the lane; using it
        // keeps the 160-byte ObjectData stride untouched, which is non-negotiable
        // (depth/shadow/velocity/probe vertex shaders all share that stride).
        float shimmerAmt = float( vTerrainPack.x        & 0xFFu) / 255.0;
        float lensAmt    = float((vTerrainPack.x >>  8) & 0xFFu) / 255.0;
        float phase      = float((vTerrainPack.x >> 16) & 0xFFu) / 255.0;
        // GlassMaterial::horizon — WHICH radial deflection profile the lens drives.
        // 0 on every surface that existed before this, so the branch below is
        // byte-identical for them.
        float horizonAmt = float((vTerrainPack.x >> 24) & 0xFFu) / 255.0;

        vec2 sUV = gl_FragCoord.xy * g.screen.xy;
        vec3 emisMaskM = mix(vec3(1.0), texel, clamp(vGlassTint.a, 0.0, 1.0));
        vec3 glow = body + vEmissive.rgb * vEmissive.a * emisMaskM;

        vec3 behind = vec3(0.0);
        if (g.screen.w > 0.5) {
            // THE SCREEN-SPACE RADIAL FRAME, taken from the UV gradient.
            // vUV.y runs radially across the annulus (0 = inner edge, facing the
            // convergence; 1 = outer rim), so its SCREEN-SPACE gradient IS the
            // outward radial direction — for any camera angle, without needing
            // the object's centre, its axis, or one extra varying. vUV.x (the
            // angle) is deliberately NOT differentiated: it wraps 0->1 at the
            // seam and its derivative explodes there.
            vec2 gradV = vec2(dFdx(vUV.y), dFdy(vUV.y));
            float g2 = dot(gradV, gradV);
            vec2 radial  = (g2 > 1e-12) ? gradV * inversesqrt(g2) : vec2(0.0, 1.0);
            vec2 tangent = vec2(-radial.y, radial.x);

            // SEAM-FREE TURBULENCE. An atan-style angular coordinate cannot be fed
            // to 2D noise — the -pi/+pi wrap leaves a visible radial seam.
            // Embedding the angle on the UNIT CIRCLE and sampling there is
            // continuous. Same trick the CPU bake uses; the most reusable line in
            // either file.
            float ang = vUV.x * 6.28318530718;
            vec2  emb = vec2(cos(ang), sin(ang)) * 3.0;
            // The per-object PHASE decorrelates neighbouring layers. Without it
            // every annulus in a stack samples the same noise at the same place,
            // and the turbulence itself lines up into concentric banding — the
            // exact artifact this pass exists to dissolve.
            emb += vec2(phase * 37.0, phase * 61.0);
            // dt-CORRECT: g.camPos.w is ACCUMULATED SECONDS off a steady_clock,
            // never a frame counter, so a 60 Hz and a 165 Hz run that reach the
            // same wall clock sample identical turbulence.
            float t = g.camPos.w;
            float nA = mFbm(emb + vec2( 1.7, vUV.y * 2.2 - t * 0.45));
            float nB = mFbm(emb + vec2(-4.3, vUV.y * 2.2 + t * 0.31));
            vec2 turb = vec2(nA, nB) * 2.0 - 1.0;              // [-1, 1]

            // LENS: strongest at the RIM (v -> 1), easing to nothing at the inner
            // edge, so the bend is a HALO around the aperture rather than a
            // uniform smear across it. Space is most distorted where the throat
            // meets flat space, and that gradient is the whole read.
            float profThroat = vUV.y * vUV.y;
            // THE EVENT-HORIZON PROFILE (horizonAmt -> 1). A surface whose INNER
            // edge sits ON a horizon and which extends outward into flat space
            // wants the OPPOSITE gradient: thin-lens deflection goes as 1/theta,
            // so it is maximal right at the horizon and dies with distance. That
            // 1/theta law is what makes an EINSTEIN RING: the deflection sweeps
            // background from a wide annulus of sky into a narrow bright band at
            // the inner rim, instead of smearing it uniformly the way a linear or
            // v^2 profile does. `v` here IS theta measured outward from the
            // horizon, so 1/(1 + 3v) is the law directly, normalised to 1 at the
            // rim.
            //
            // NORMALISED TO 1 AT THE RIM, deliberately the same peak as the
            // throat profile. It was 3.2 for one capture round and that was a
            // mistake with a visible signature: 3.2 x the throat's peak, on top of
            // a master refraction already 2.6x the throat's, put ~10% OF THE
            // SCREEN of displacement into the halo. The result was not a lens, it
            // was a grey smear disc — the whole band sampling the same distant
            // patch of sky. The magnitude belongs in kHaloRefract, where one
            // number governs it and r_glass_refract can scrub it; the profile's
            // job is only the SHAPE.
            float profHorizon = 1.0 / (1.0 + 3.0 * vUV.y);
            float prof = mix(profThroat, profHorizon, horizonAmt);
            // Sampling INWARD is what makes the background appear pushed OUTWARD,
            // which is the direction light actually deflects around a mass. Same
            // sign for both profiles — only the magnitude law differs.
            vec2 off = -radial * (lensAmt * 1.15 * prof)     // pull the scene inward
                     +  tangent * (lensAmt * 0.55 * prof);   // + a swirl drag
            // SHIMMER in the membrane's OWN radial/tangential frame, so the
            // turbulence follows the throat instead of the screen axes.
            off += (radial * turb.x + tangent * turb.y) * (shimmerAmt * 0.85);
            // `refraction` is the master scale (and scrubs live via r_glass_refract).
            vec2 sampUV = clamp(sUV + off * refraction, vec2(0.0), vec2(1.0));

            vec3 sharp = textureLod(sceneCopy, sampUV, 0.0).rgb;
            if (g.screen.z > 0.5 && roughness > 0.001) {
                vec3 frost = textureLod(sceneFrost, sampUV, 0.0).rgb;
                sharp = mix(sharp, frost, smoothstep(0.0, 0.85, roughness));
            }
            behind = sharp * mix(vec3(1.0), tint, 0.75);
        }
        // else: GRACEFUL DEGRADATION. No scene copy this frame (target creation
        // failed, or the copy pass was skipped) -> there is nothing to bend, so
        // the membrane falls back to its own emissive body over whatever the
        // alpha blend reveals. Flatter, but never black and never a garbage tap.

        // ALPHA-BLENDED, NOT REPLACED. The pane path outputs alpha 1 and REPLACES
        // the pixel, which is right for one sheet of glass — but a stack of
        // membranes must COMPOSITE, and a replace lets the nearest layer paint
        // over every layer behind it. That is precisely the opaque-overwrite
        // mechanism that made 30 stacked annuli read as discrete concentric
        // hoops. Under SRC_ALPHA/ONE_MINUS_SRC_ALPHA the result here is
        //     a*behind + glow + (1-a)*dst
        // so the glow is pre-divided by a to survive the blend's multiply.
        // NOTE ON THE GRAZING FADE. The side-on "discrete hoops" read is fixed on
        // the CPU (Wormhole::facingFade, app/space/wormhole.cpp), not here. A
        // per-fragment dot(N,V) is the wrong instrument: the throat is 42 m long,
        // so from a side-on camera 110 m away the MOUTH is edge-on while the
        // convergence is still 20 degrees open, and a per-fragment fade therefore
        // dissolves the near rings while leaving the far ones — which is a
        // stranger artifact than the one it replaces. The whole aperture has to
        // fade as ONE object, off ONE angle measured at its axis, and the CPU is
        // where that angle is known exactly. It arrives here already folded into
        // opacity and into the base-colour drive.
        // The glow is pre-divided by alpha so the blend's multiply gives it back
        // (see above). The DIVISOR is floored but the OUTPUT ALPHA is not: flooring
        // the alpha itself breaks the cancellation, and a membrane faded to zero
        // coverage would emit glow/floor — a 250x amplification that resurrects
        // every ring as a bright outline exactly where it was supposed to vanish.
        // Alpha 0 must mean gone, whatever the numerator says.
        float aRaw = clamp(vFactor.a, 0.0, 1.0);
        float aDiv = max(aRaw, 0.01);
        outColor = vec4(behind + glow / aDiv, aRaw);
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
    float shadow = sampleShadow(vWorldPos, N, ndl);
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

    // Point lights via the SHARED iterator (inc/mesh_lighting.glsl): the legacy
    // 64-entry UBO array when r_clusterlights is 0, this fragment's froxel list
    // when it is 1 — so a glass pane in Echo Harbor's neon night now catches the
    // same 1024-light set the opaque facade beside it does.
    int n = x3LightCount();
    for (int i = 0; i < n; ++i) {
        ClusterLight PL = x3Light(i);
        vec3  toL  = PL.posRange.xyz - vWorldPos;
        float dist = length(toL);
        vec3  L    = toL / max(dist, 0.0001);
        float pndl = max(dot(N, L), 0.0);
        float att  = pointAtten(dist, PL.posRange.w)
                   * spotCone(PL.dirCone, PL.colorPad.a, L);
        lighting  += PL.colorPad.rgb * (pndl * att);
        specSum   += specLight(N, V, L, PL.colorPad.rgb, specRough) * att;
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
