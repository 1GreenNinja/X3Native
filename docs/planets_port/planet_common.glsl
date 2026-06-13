// =============================================================================
//  planet_common.glsl  —  shared GLSL 450 library for the X3Native planet ports
//  Source: FORGE3D "Planets HD" (Unity / Amplify Shader Editor) ports.
//
//  This is the ONE clean, deduped copy of every helper/noise/math primitive that
//  the per-type planet_<type>.frag shaders reference. The X3Native shader build
//  has no real #include, so during SPIR-V compilation either:
//    (a) textually paste this file at the top of each .frag (a tiny preprocessor
//        step in the asset pipeline — recommended), OR
//    (b) add `#extension GL_GOOGLE_include_directive : require` + a glslang
//        include path and `#include "planet_common.glsl"`.
//  Each planet_<type>.frag begins with the comment marker:  // #include "planet_common.glsl"
//
//  CONVENTIONS (engine-wide, see ARCHITECTURE.md):
//    uSunDir  : world-space direction TOWARD the light (normalized)
//    uCamPos  : world-space camera position
//    uTime    : seconds (replaces Unity _Time.y;  _Time.x == uTime/20.0)
//    All triplanar sampling is in OBJECT space on a unit sphere (objPos==objNormalDir).
//
//  NOTE ON NOISE: the FORGE3D pack is almost entirely TEXTURE-DRIVEN — none of the
//  ported shaders use analytic Simplex/Voronoi/Perlin lattice noise. The "noise"
//  is precomputed grayscale/flow textures sampled via triplanar + polar + flow-map
//  distortion. The analytic noise functions below (value/gradient/simplex/voronoi/
//  fbm) are therefore provided as a PORTABLE FALLBACK so a planet can run with NO
//  bound textures (procedural placeholder look) and for future procedural variants.
//  The faithful ports use the TEXTURE path; the analytic path is opt-in.
// =============================================================================

#ifndef PLANET_COMMON_GLSL
#define PLANET_COMMON_GLSL

const float PI      = 3.14159265358979323846;
const float TWO_PI  = 6.28318530717958647692;
const float INV_2PI = 0.15915494309189533577;

// ---------------------------------------------------------------------------
//  saturate / misc
// ---------------------------------------------------------------------------
float saturate1(float x){ return clamp(x, 0.0, 1.0); }
vec2  saturate2(vec2  x){ return clamp(x, 0.0, 1.0); }
vec3  saturate3(vec3  x){ return clamp(x, 0.0, 1.0); }
vec4  saturate4(vec4  x){ return clamp(x, 0.0, 1.0); }

// Unity-style ST application: uv * scale + offset  (vec4 ST = scale.xy, offset.zw)
vec2 applyST(vec2 uv, vec4 st){ return uv * st.xy + st.zw; }

// ASE "Linstep": saturate((x-a)/(b-a))
float linstep(float a, float b, float x){ return clamp((x - a) / (b - a), 0.0, 1.0); }

// ASE "Ramp3": two-stage clamped gradient. t is pre-divided by the scale at call site.
vec3 ramp3(vec3 low, vec3 mid, vec3 high, float t){
    vec3 c = mix(low, mid,  clamp(t,       0.0, 1.0));
    return  mix(c,   high, clamp(t - 1.0, 0.0, 1.0));
}

// ---------------------------------------------------------------------------
//  Lat-long UV from a (object- or world-) space sphere normal/direction.
//  u = longitude in [0,1) (atan2 wrapped), v = latitude in [0,1] (acos).
//  Matches a standard UV-sphere lat-long parameterisation; use when a port wants
//  vUV but you only have the direction (or to recompute seam-free in the frag).
// ---------------------------------------------------------------------------
vec2 latLongUV(vec3 dir){
    dir = normalize(dir);
    float u = atan(dir.z, dir.x) * INV_2PI + 0.5;   // [0,1)
    float v = acos(clamp(dir.y, -1.0, 1.0)) / PI;   // [0,1] pole->pole
    return vec2(u, v);
}

// ---------------------------------------------------------------------------
//  TANGENT-SPACE NORMAL unpack (Unity UnpackScaleNormal).
//  FORGE3D ships plain RGB normal maps (xy in .rg). If a map is DXT5nm (xy in
//  .ag) define PLANET_NORMAL_AG before including. Z is reconstructed.
// ---------------------------------------------------------------------------
vec3 unpackScaleNormal(vec4 packed, float scale){
    vec3 n;
#ifdef PLANET_NORMAL_AG
    n.xy = (packed.ag * 2.0 - 1.0) * scale;
#else
    n.xy = (packed.rg * 2.0 - 1.0) * scale;
#endif
    n.z  = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// Build a stable tangent frame from a (world or object) normal, for applying a
// tangent-space normal map when no mesh TBN is supplied. Prefer the real vTBN
// passed from planet.vert when available (seam-free at the poles).
mat3 sphereTBN(vec3 N){
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T  = normalize(cross(up, N));
    vec3 B  = cross(N, T);
    return mat3(T, B, N);   // columns: tangent, bitangent, normal (tangent->world)
}

// ---------------------------------------------------------------------------
//  FRESNEL rim.  rim = saturate(pow(saturate(1 - dot(V,N)), power)) * mult.
//  V toward camera, N normalized. Returns the scalar rim factor; tint at call.
// ---------------------------------------------------------------------------
float fresnelRim(vec3 V, vec3 N, float power, float mult){
    float d = dot(normalize(V), normalize(N));
    return clamp(pow(clamp(1.0 - d, 0.0, 1.0), power), 0.0, 1.0) * mult;
}

// ---------------------------------------------------------------------------
//  OBJECT-SPACE TRIPLANAR sampling (ASE "Triplanar", Spherical/Object variant).
//  Every FORGE3D planet body uses this. projN = pow(abs(objNormal), falloff),
//  normalized; per-plane UVs flip sign by sign(objNormal) component exactly as
//  ASE emits. `uvOffset` carries the animated scroll/flow offset (0 if static).
//  Falloff is 5.0 for almost every map (lava normal uses 1.0, lava mask 2.0).
// ---------------------------------------------------------------------------
vec4 triplanar(sampler2D tex, vec3 objPos, vec3 objNormal,
               float falloff, float tiling, vec2 uvOffset){
    vec3 w = pow(abs(objNormal), vec3(falloff));
    w /= max(w.x + w.y + w.z, 1e-5);
    vec3 s = sign(objNormal);
    vec2 uvX = objPos.zy * vec2( s.x, 1.0) * tiling + uvOffset;
    vec2 uvY = tiling * objPos.xz * vec2( s.y, 1.0) + uvOffset;
    vec2 uvZ = uvOffset + tiling * objPos.xy * vec2(-s.z, 1.0);
    return w.x * texture(tex, uvX) + w.y * texture(tex, uvY) + w.z * texture(tex, uvZ);
}
// Convenience: no animation offset.
vec4 triplanar(sampler2D tex, vec3 objPos, vec3 objNormal, float falloff, float tiling){
    return triplanar(tex, objPos, objNormal, falloff, tiling, vec2(0.0));
}

// ---------------------------------------------------------------------------
//  ROTATE UV about (0.5,0.5) by angle = speed*time (ASE "Rotator"/"rotateUV").
//  The ASE *0.5+0.5 then *2-1 matrix dance collapses to a plain rotation.
// ---------------------------------------------------------------------------
vec2 rotateUV(vec2 uv, float speed, float time){
    uv -= 0.5;
    float s = sin(speed * time);
    float c = cos(speed * time);
    vec2 r;
    r.x = uv.x * c + uv.y * s;     // ASE HLSL mul(rowVec, ((c,-s),(s,c)))
    r.y = uv.x * (-s) + uv.y * c;
    return r + 0.5;
}

// Panner (ASE "Panner"): uv += time*speed*dir.
vec2 panner(vec2 uv, vec2 dir, float speed, float time){ return uv + (speed * time) * dir; }

// ---------------------------------------------------------------------------
//  POLAR COORDINATES (ASE "Polar Coordinates"). Input uv2 = uv*2 (centered to
//  ~[-1,1] via -1). Returns (tileX*angle01, tileY*radius). Used by SunCorona.
// ---------------------------------------------------------------------------
vec2 polarCoord(vec2 uv2, float tileX, float tileY){
    vec2 p = uv2 - 1.0;
    float a = -atan(p.y, p.x);
    float ang = a * INV_2PI;
    if (a < 0.0) ang = (a + TWO_PI) * INV_2PI;     // wrap to [0,1)
    return vec2(tileX * ang, tileY * length(p));
}

// ---------------------------------------------------------------------------
//  FLOW-MAP / UV self-distortion. Sample a (triplanar) flow texture, saturate,
//  scale by `factor`; its RG warps the UV of a second lookup. This single-pass
//  warp is how Gas/Lava/Sun/Thunderstorm produce "swirling" motion.
//  baseUV = scrollOffset + warpRG.
// ---------------------------------------------------------------------------
vec2 flowDistortOffset(sampler2D flowTex, vec3 objPos, vec3 objNormal,
                       float tiling, float falloff, vec2 flowScroll, float factor){
    vec4 f = clamp(triplanar(flowTex, objPos, objNormal, falloff, tiling, flowScroll), 0.0, 1.0);
    return f.rg * factor;
}

// =============================================================================
//  ATMOSPHERIC SCATTER LUT — the single most reused planet idiom.
//  Almost every surface planet (Gas/Ice/Lava/Oceanic/Sand/Terrestrial/
//  Thunderstorm/Moon) ends with: build a 2D LUT coord from (N.L remapped, N.V),
//  sample _ScatterMap, tint by color*light, *boost, +indirect, then MULTIPLY the
//  lit surface result by it (a day/terminator/limb atmospheric tint).
//
//    coord = ( centerShift + vec2(dot(N,L)*0.5+0.5, dot(N,V)) ) * stretch
//    s     = saturate( tex(scatterMap, coord).rgb * color * light )
//    s     = saturate( s * boost )
//    s     = saturate( s + indirect )
//  N here is the GEOMETRIC world normal (NOT the perturbed bump normal).
// =============================================================================
vec3 scatterTerm(sampler2D scatterMap, vec3 N, vec3 L, vec3 V,
                 float centerShift, float stretch, vec3 color, vec3 light,
                 float boost, float indirect){
    vec2 coord = (vec2(centerShift) + vec2(dot(N, L) * 0.5 + 0.5, dot(N, V))) * stretch;
    vec3 s = saturate3(texture(scatterMap, coord).rgb * color * light);
    s = saturate3(s * boost);
    s = saturate3(s + vec3(indirect));
    return s;
}

// =============================================================================
//  CLOUD LAYER (two-layer animated) — shared by Oceanic / Sand / Terrestrial.
//  Pole layer (CloudsTop): UV rotated about center at topSpeed*time.
//  Belly layer (CloudsMiddle): UV scrolled in U at midSpeed*time.
//  Blended by a gradient ramp ^ blendWeight. Plus a self-shadow term re-sampling
//  each layer offset by 0.005*lightDir. Returns cloud color (rgb) + shadow (out).
//  worldToTangent transforms the world light dir into tangent space for the belly
//  shadow offset (pass transpose(vTBN)). Lat-long uv is the cloud UV.
// =============================================================================
void cloudsTwoLayer(
    sampler2D cloudsTop, sampler2D cloudsMiddle, sampler2D gradient,
    vec2 uv, vec4 topST, vec4 midST, vec4 gradST,
    float topSpeed, float midSpeed, float blendWeight,
    vec3 cloudsTint, float cloudsBoost, float cloudShadows,
    vec3 worldLightDir, mat3 worldToTangent, float time,
    out vec3 cloudColor, out float cloudShadow)
{
    vec2 uvTop  = applyST(uv, topST);
    vec2 poleUV = rotateUV(uvTop, topSpeed, time);
    float cloudPole = texture(cloudsTop, poleUV).r;

    vec2 uvMid   = applyST(uv, midST);
    vec2 bellyUV = vec2(uvMid.x + midSpeed * time, uvMid.y);
    float cloudBelly = texture(cloudsMiddle, bellyUV).r;

    vec2 uvGrad = applyST(uv, gradST);
    float gradientMap = pow(texture(gradient, uvGrad).r, blendWeight);

    float cloudMix = mix(cloudPole, cloudBelly, gradientMap);
    cloudColor = saturate3(vec3(cloudMix) * cloudsTint * cloudsBoost);

    // Self-shadow: offset-sample each layer toward the light, blend, ramp.
    vec2 shadowUVPole = vec2(0.005 * worldLightDir.x);
    float poleShadow  = texture(cloudsTop, poleUV + shadowUVPole).r;
    vec3  Ltan        = normalize(worldToTangent * worldLightDir);
    vec2  shadowUVBelly = vec2(Ltan.x * 0.005);
    float bellyShadow = texture(cloudsMiddle, bellyUV + shadowUVBelly).r;
    float mixShadow   = mix(poleShadow, bellyShadow, gradientMap + 0.1);
    cloudShadow = clamp(pow(1.0 - mixShadow, cloudShadows * 50.0), 0.0, 1.0);
}

// =============================================================================
//  LIGHTING — compact Unity LightingStandardSpecular stand-in (single sun).
//  specColor = F0 (specular workflow), smoothness in [0,1] (roughness=1-smooth).
//  ambient is a flat GI fill (X3Native has SSGI/SSAO in the main pass; for a
//  standalone planet pass pass a small constant or your engine ambient).
// =============================================================================
float D_GGX(float NoH, float a){
    float a2 = a * a;
    float d  = (NoH * NoH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}
float V_SmithGGX(float NoV, float NoL, float a){
    float k = (a * a) * 0.5;
    float gv = NoV / (NoV * (1.0 - k) + k);
    float gl = NoL / (NoL * (1.0 - k) + k);
    return gv * gl;
}
vec3 F_Schlick(vec3 f0, float u){ return f0 + (vec3(1.0) - f0) * pow(1.0 - u, 5.0); }

vec3 standardSpecular(vec3 albedo, vec3 specColor, float smoothness,
                      vec3 N, vec3 V, vec3 L, vec3 lightColor, vec3 ambient){
    float rough = max(1.0 - clamp(smoothness, 0.0, 1.0), 0.045);
    vec3  H   = normalize(L + V);
    float NoL = max(dot(N, L), 0.0);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);
    float oneMinusRefl = 1.0 - max(max(specColor.r, specColor.g), specColor.b);
    vec3  diffuse  = albedo * oneMinusRefl * NoL;
    vec3  specular = (D_GGX(NoH, rough) * V_SmithGGX(NoV, NoL, rough)) * F_Schlick(specColor, VoH) * NoL;
    return (diffuse + specular) * lightColor + albedo * ambient;
}

// =============================================================================
//  ANALYTIC NOISE (PORTABLE FALLBACK — not used by the faithful texture ports).
//  Provided so a planet can render procedurally with no bound textures, and for
//  future procedural variants. Hash-based value+gradient noise, a 3D simplex,
//  Voronoi (F1/F2), and fbm. All deterministic, tileable-ish in object space.
// =============================================================================

// Integer-ish hash -> [0,1)
float hash11(float p){ p = fract(p * 0.1031); p *= p + 33.33; p *= p + p; return fract(p); }
vec3  hash33(vec3 p){
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123);
}
float hash13(vec3 p){
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// 3D value noise (smooth interpolated lattice).
float valueNoise(vec3 p){
    vec3 i = floor(p), f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + vec3(0,0,0)), n100 = hash13(i + vec3(1,0,0));
    float n010 = hash13(i + vec3(0,1,0)), n110 = hash13(i + vec3(1,1,0));
    float n001 = hash13(i + vec3(0,0,1)), n101 = hash13(i + vec3(1,0,1));
    float n011 = hash13(i + vec3(0,1,1)), n111 = hash13(i + vec3(1,1,1));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// 3D gradient (Perlin-style) noise in ~[-1,1].
float gradientNoise(vec3 p){
    vec3 i = floor(p), f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0); // quintic
    #define GDOT(o) dot(hash33(i + o) * 2.0 - 1.0, f - o)
    float v = mix(mix(mix(GDOT(vec3(0,0,0)), GDOT(vec3(1,0,0)), u.x),
                      mix(GDOT(vec3(0,1,0)), GDOT(vec3(1,1,0)), u.x), u.y),
                  mix(mix(GDOT(vec3(0,0,1)), GDOT(vec3(1,0,1)), u.x),
                      mix(GDOT(vec3(0,1,1)), GDOT(vec3(1,1,1)), u.x), u.y), u.z);
    #undef GDOT
    return v;
}

// 3D simplex noise (Ashima/McEwan), output ~[-1,1].
vec4 mod289v4(vec4 x){ return x - floor(x * (1.0/289.0)) * 289.0; }
vec3 mod289v3(vec3 x){ return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 permute(vec4 x){ return mod289v4(((x*34.0)+1.0)*x); }
vec4 taylorInvSqrt(vec4 r){ return 1.79284291400159 - 0.85373472095314 * r; }
float simplexNoise(vec3 v){
    const vec2 C = vec2(1.0/6.0, 1.0/3.0);
    const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);
    vec3 i  = floor(v + dot(v, C.yyy));
    vec3 x0 = v - i + dot(i, C.xxx);
    vec3 g = step(x0.yzx, x0.xyz);
    vec3 l = 1.0 - g;
    vec3 i1 = min(g.xyz, l.zxy);
    vec3 i2 = max(g.xyz, l.zxy);
    vec3 x1 = x0 - i1 + C.xxx;
    vec3 x2 = x0 - i2 + C.yyy;
    vec3 x3 = x0 - D.yyy;
    i = mod289v3(i);
    vec4 p = permute(permute(permute(
                 i.z + vec4(0.0, i1.z, i2.z, 1.0))
               + i.y + vec4(0.0, i1.y, i2.y, 1.0))
               + i.x + vec4(0.0, i1.x, i2.x, 1.0));
    float n_ = 0.142857142857;
    vec3 ns = n_ * D.wyz - D.xzx;
    vec4 j = p - 49.0 * floor(p * ns.z * ns.z);
    vec4 x_ = floor(j * ns.z);
    vec4 y_ = floor(j - 7.0 * x_);
    vec4 x = x_ * ns.x + ns.yyyy;
    vec4 y = y_ * ns.x + ns.yyyy;
    vec4 h = 1.0 - abs(x) - abs(y);
    vec4 b0 = vec4(x.xy, y.xy);
    vec4 b1 = vec4(x.zw, y.zw);
    vec4 s0 = floor(b0) * 2.0 + 1.0;
    vec4 s1 = floor(b1) * 2.0 + 1.0;
    vec4 sh = -step(h, vec4(0.0));
    vec4 a0 = b0.xzyw + s0.xzyw * sh.xxyy;
    vec4 a1 = b1.xzyw + s1.xzyw * sh.zzww;
    vec3 p0 = vec3(a0.xy, h.x);
    vec3 p1 = vec3(a0.zw, h.y);
    vec3 p2 = vec3(a1.xy, h.z);
    vec3 p3 = vec3(a1.zw, h.w);
    vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));
    p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
    vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
    m = m * m;
    return 42.0 * dot(m * m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}

// 3D Voronoi (cellular). Returns vec2(F1, F2): nearest + second-nearest distance.
vec2 voronoi(vec3 p){
    vec3 b = floor(p), f = fract(p);
    float f1 = 8.0, f2 = 8.0;
    for (int k = -1; k <= 1; ++k)
    for (int j = -1; j <= 1; ++j)
    for (int i = -1; i <= 1; ++i){
        vec3 g = vec3(float(i), float(j), float(k));
        vec3 o = hash33(b + g);
        vec3 r = g + o - f;
        float d = dot(r, r);
        if (d < f1){ f2 = f1; f1 = d; }
        else if (d < f2){ f2 = d; }
    }
    return sqrt(vec2(f1, f2));
}

// fBm over any base noise (defaults to gradientNoise). Octaves, lacunarity, gain.
float fbm(vec3 p, int octaves, float lacunarity, float gain){
    float sum = 0.0, amp = 0.5, freq = 1.0;
    for (int i = 0; i < octaves; ++i){
        sum  += amp * gradientNoise(p * freq);
        freq *= lacunarity;
        amp  *= gain;
    }
    return sum;
}
float fbm(vec3 p){ return fbm(p, 5, 2.0, 0.5); }

// Ridged fbm (mountainous/turbulent — useful for procedural lava/ice fallback).
float ridgedFbm(vec3 p, int octaves, float lacunarity, float gain){
    float sum = 0.0, amp = 0.5, freq = 1.0;
    for (int i = 0; i < octaves; ++i){
        float n = 1.0 - abs(gradientNoise(p * freq));
        n *= n;
        sum  += amp * n;
        freq *= lacunarity;
        amp  *= gain;
    }
    return sum;
}

#endif // PLANET_COMMON_GLSL
