#version 450

// VOLUMETRIC LIGHT SCATTERING — CLEAN-ROOM, original. ART_BIBLE.md §5 (extends
// the flat depth-fog pass into a raymarched participating medium).
//
// WHERE IT RUNS: exactly where fog.frag runs — a fullscreen pass over the finished
// HDR scene, AFTER the last HDR writer (particles) and BEFORE the TAA resolve, so
// the scattered radiance enters temporal history + bloom like real scene light.
// This shader REPLACES fog.frag only when the host set FogParams::volumetric; the
// flat shader/pipeline is untouched and still used otherwise (byte-identical).
//
// WHAT IT ADDS over Beer-Lambert extinction:
//   * SUN SHAFTS — each march step samples the SAME sun shadow map the meshes
//     receive (set2/binding0, hardware-compare sampler2DShadow, projected by
//     cam.lightViewProj from set1/binding1). Unshadowed steps scatter sun light,
//     shadowed ones don't -> god rays through gaps in the skyline.
//   * NEON / LAMP HAZE — the frame's forward point lights (cam.lights[], the same
//     array mesh.frag shades with) scatter into the air, so every sign and street
//     lamp wears a real cone/halo of haze.
//   * HENYEY-GREENSTEIN phase — forward-scattering anisotropy, so looking toward a
//     light source blooms hard and looking away stays subtle. This is the term that
//     sells the effect.
//
// BLENDING: this pipeline uses PREMULTIPLIED alpha (ONE, ONE_MINUS_SRC_ALPHA), i.e.
//   dst = inscatter + dst * (1 - f).
// Writing rgb = fogColor*f (and nothing else) reproduces the flat pass's
// mix(scene, fogColor, f) EXACTLY, so scatterStrength == 0 degrades to today's fog.
//
// BANDING: a low-discrepancy per-pixel dither offsets each ray's start within one
// step (interleaved-gradient noise + a golden-ratio per-frame rotation), which turns
// the classic raymarch "onion rings" into high-frequency noise that TAA integrates
// away over its 8-frame Halton cycle.

layout(set = 0, binding = 0) uniform sampler2D depthTex;   // main depth (D32, [0,1])

// ---- set 1: the SHARED per-frame object/camera set (m_objSetLayout). Binding 0/2
// are the vertex-stage object SSBOs (not declared here — a fragment shader may
// leave set bindings it doesn't use undeclared). Binding 1 is the Camera UBO; the
// block below must match FrameUBO / mesh.frag's Camera block byte-for-byte.
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
    vec4 camPos;                    // xyz = camera world position
    vec4 sunDir;                    // xyz = direction TOWARD the sun, w = sun radiance scale
} cam;

// ---- set 2: the sun shadow map (same set the mesh pass binds).
layout(set = 2, binding = 0) uniform sampler2DShadow shadowMap;

layout(push_constant) uniform Push {
    mat4  invViewProj;    // jittered clip -> WORLD (matches this frame's depth buffer)
    vec4  colorDensity;   // rgb = fog color (linear HDR), w = extinction density (1/m)
    vec4  startMax;       // x = start distance (m), y = max opacity, zw = pad
    vec4  vol0;           // x = scatterStrength (1/m), y = anisotropy g,
                          // z = steps, w = max march distance (m)
    vec4  vol1;           // x = sun scatter mul, y = point-light scatter mul,
                          // z = frame seed (dither rotation), w = pad
} pc;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;
const vec3  kSunColor = vec3(1.0, 0.97, 0.92);   // same warm white mesh.frag keys with

// Henyey-Greenstein phase function. g > 0 = forward scattering (haze/aerosols):
// large toward the light, small away from it. Normalised over the sphere.
float hg(float cosT, float g) {
    float g2 = g * g;
    float d  = max(1.0 + g2 - 2.0 * g * cosT, 1e-4);
    return (1.0 - g2) / (4.0 * PI * d * sqrt(d));
}
// Real urban haze is a MIXTURE — a strongly forward aerosol lobe plus a near-
// isotropic one. A pure HG at g = 0.76 is so peaked that a lamp gets a bright
// spike dead-on and literally nothing to the side, which reads as a lens artifact
// instead of a cone of lit air. Blending in an isotropic floor keeps the halo
// present all around the fixture while the forward lobe still does the "looking
// toward the light blooms" work.
const float kForwardLobe = 0.65;
float phaseFn(float cosT, float g) {
    return mix(0.25 / PI, hg(cosT, g), kForwardLobe);
}

// Same windowed falloff mesh.frag uses for point lights, so the haze around a lamp
// dies exactly where the lamp's surface lighting does (no halo past its range).
float pointAtten(float dist, float range) {
    float t = dist / max(range, 0.0001);
    float w = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    w *= w;
    return w / (dist * dist + 1.0);
}

// Single hardware-compare tap into the sun shadow map at a WORLD point. Outside the
// shadow frustum -> lit (the sampler's border is OPAQUE_WHITE, matching mesh.frag).
float sunVisibility(vec3 worldPos) {
    vec4 lc = cam.lightViewProj * vec4(worldPos, 1.0);
    vec3 proj = lc.xyz / lc.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0) return 1.0;
    // Constant depth bias: there is no surface normal in the medium, so the
    // slope-scaled bias mesh.frag uses has no meaning here.
    return texture(shadowMap, vec3(uv, proj.z - 0.0015));
}

// Interleaved-gradient noise (screen-space, low discrepancy) + a golden-ratio
// per-frame rotation so successive TAA samples land on different offsets.
float dither(vec2 pix, float seed) {
    float n = fract(52.9829189 * fract(dot(pix, vec2(0.06711056, 0.00583715))));
    return fract(n + seed * 0.61803399);
}

vec3 worldFromClip(vec2 uv, float depth) {
    vec4 w = pc.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return w.xyz / w.w;
}

void main() {
    const float density = pc.colorDensity.w;
    const float start   = pc.startMax.x;

    float depth = texture(depthTex, vUV).r;
    vec3  camW  = cam.camPos.xyz;
    vec3  farW  = worldFromClip(vUV, 1.0);
    vec3  dir   = normalize(farW - camW);

    // Scene distance along the ray. min(depth, 0.99999) is fog.frag's far-plane
    // guard VERBATIM: sky pixels get the full path length (and so the maxOpacity
    // cap), which is what makes the night sky read as haze-tinted rather than void.
    // The raymarch clamps this SEPARATELY below — the flat law must not be touched.
    float sceneDist = length(worldFromClip(vUV, min(depth, 0.99999)) - camW);

    // ---- The FLAT term, bit-for-bit the fog.frag law (premultiplied here). -----
    float f = 1.0 - exp(-density * max(sceneDist - start, 0.0));
    f = min(f, pc.startMax.y);
    vec3  outRgb = pc.colorDensity.rgb * f;

    // ---- The RAYMARCHED in-scatter -------------------------------------------
    float sigmaS = pc.vol0.x;
    if (sigmaS > 0.0) {
        float g      = pc.vol0.y;
        int   steps  = clamp(int(pc.vol0.z), 4, 64);
        float tEnd   = min(sceneDist, pc.vol0.w);

        if (tEnd > 0.0) {
            // Per-light pre-cull: a lamp only matters if the ray passes within its
            // range. 64 cheap tests up front collapse the inner loop from 64 lights
            // per step to the handful that actually touch this pixel's ray.
            const int kMaxRayLights = 24;
            int  lidx[kMaxRayLights];
            int  lcount = 0;
            int  nLights = int(cam.ambientCount.w);
            for (int i = 0; i < nLights && i < kMaxPointLights && lcount < kMaxRayLights; ++i) {
                vec3  toL = cam.lights[i].posRange.xyz - camW;
                float tc  = clamp(dot(toL, dir), 0.0, tEnd);
                vec3  perp = toL - dir * tc;
                float r    = cam.lights[i].posRange.w;
                if (dot(perp, perp) < r * r) lidx[lcount++] = i;
            }

            vec3  sunRad   = kSunColor * max(cam.sunDir.w, 0.0) * pc.vol1.x;
            vec3  sunL     = normalize(cam.sunDir.xyz);
            float sunPhase = phaseFn(dot(dir, sunL), g);
            bool  doSun    = (pc.vol1.x > 0.0) && (cam.sunDir.w > 0.0);
            float lightMul = pc.vol1.y;

            float jitter = dither(gl_FragCoord.xy, pc.vol1.z);
            // NON-UNIFORM (power-distributed) steps. A street lamp's range is 9-15 m
            // but a city vista's ray is hundreds of metres long; uniform steps would
            // put one sample inside a lamp's whole halo and alias it to sparkle.
            // t(s) = tEnd * (s/steps)^kMarchPow packs samples near the camera — where
            // the lamps are and where the medium matters most — while still covering
            // the full depth for the sun shafts. Each segment carries its OWN length,
            // so the integral (and the Beer-Lambert transmittance) stays correct.
            const float kMarchPow = 2.0;
            float invSteps = 1.0 / float(steps);
            vec3  inscat = vec3(0.0);
            float T      = 1.0;
            float tPrev  = 0.0;

            for (int s = 0; s < steps; ++s) {
                float tNext = tEnd * pow(float(s + 1) * invSteps, kMarchPow);
                float dtSeg = tNext - tPrev;
                float t     = tPrev + jitter * dtSeg;   // dithered position INSIDE the segment
                tPrev       = tNext;
                if (dtSeg <= 0.0) continue;
                vec3  p = camW + dir * t;

                vec3 L = vec3(0.0);
                if (doSun) L += sunRad * sunVisibility(p) * sunPhase;
                for (int k = 0; k < lcount; ++k) {
                    int   i    = lidx[k];
                    vec3  toL  = cam.lights[i].posRange.xyz - p;
                    float d    = length(toL);
                    float att  = pointAtten(d, cam.lights[i].posRange.w);
                    if (att <= 0.0) continue;
                    L += cam.lights[i].colorPad.rgb * att
                       * phaseFn(dot(dir, toL / max(d, 1e-4)), g) * lightMul;
                }

                inscat += L * (sigmaS * T * dtSeg);
                T *= exp(-density * dtSeg);
            }
            outRgb += inscat;
        }
    }

    outColor = vec4(outRgb, f);
}
