#version 450

// Animated water-surface fragment shader (undersea-world foundation).
//
// CLEAN-ROOM, original work. Built from public water-rendering references
// (GPU Gems water chapters, Tessendorf "Simulating Ocean Water", and public
// real-time ocean articles). No game-engine source was consulted.
//
// Shaded in LINEAR HDR, pre-tonemap (the shared ACES curve runs once in
// composite.frag, after bloom), so the water sits on the same response curve as
// the sky + lit geometry and its sun glint feeds the bloom chain.
//
// Model:
//   * REFLECTION: the analytic sky color sampled in the mirror-reflected view
//     direction (same zenith->horizon gradient + sun disk/glow as sky.frag), so
//     the water mirrors the actual sky without a reflection render target.
//   * REFRACTION / DEPTH COLOR: a shallow->deep gradient driven by the WATER
//     DEPTH (how much water the view ray passes through before hitting the scene
//     floor) reconstructed from the scene depth buffer vs. this fragment's depth.
//   * FRESNEL blends refraction (face-on) -> reflection (grazing).
//   * SUN GLINT: a sharp specular lobe toward the sun (HDR, drives bloom).
//   * A subtle high-frequency RIPPLE perturbs the normal for sparkle.
//   * HORIZON FOG: far water fades into the sky color so the sea meets the sky
//     cleanly with no hard line.

layout(set = 0, binding = 0) uniform WaterUBO {
    mat4  viewProj;
    vec4  camPos;
    vec4  sunDir;
    vec4  deepColor;
    vec4  shallowColor;
    vec4  p0;   // x=seaLevel, y=time, z=amplitude, w=steepness
    vec4  p1;   // x=baseWavelength, y=speed, z=specular, w=fresnelBase
    vec4  p2;   // x=patchHalfExtent, y=1/screenW, z=1/screenH, w=reserved
} u;

// Scene depth buffer (the SSAO depth pre-pass output). Sampled as data (R32F via
// the depth aspect) to recover the opaque-scene depth behind the water.
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vGrid;

layout(location = 0) out vec4 outColor;

// --- Analytic sky color for a world-space ray (kept in sync with sky.frag's
// gradient + sun so the water's reflection matches the actual sky). ---
vec3 skyColor(vec3 dir, vec3 sunDir) {
    float up = dir.y;
    float t  = clamp(up, 0.0, 1.0);
    const vec3 kZenith  = vec3(0.10, 0.28, 0.66);
    const vec3 kHorizon = vec3(0.62, 0.74, 0.92);
    float grad = pow(t, 0.55);
    vec3 col = mix(kHorizon, kZenith, grad);
    float horizonBand = pow(1.0 - t, 8.0);
    col = mix(col, vec3(0.78, 0.82, 0.88), horizonBand * 0.55);
    // Sun disk + Mie-like glow (matches sky.frag).
    float cosA = clamp(dot(dir, sunDir), -1.0, 1.0);
    vec3 sunRGB = vec3(1.0, 0.97, 0.92);
    float glow = pow(max(cosA, 0.0), 8.0) * 0.20 + pow(max(cosA, 0.0), 256.0) * 0.55;
    col += sunRGB * glow;
    float disk = smoothstep(0.99965, 0.99986, cosA);
    col += sunRGB * disk * 4.0;
    return max(col, vec3(0.0));
}

// Reconstruct linear eye-space distance from a [0,1] clip depth (Vulkan reverse-Y
// perspective with near/far below). Returns +distance in front of the camera.
const float kNear = 0.1;
const float kFar  = 200.0;
float linearizeDepth(float d) {
    // d in [0,1], GLM_FORCE_DEPTH_ZERO_TO_ONE perspective.
    return (kNear * kFar) / (kFar - d * (kFar - kNear));
}

void main() {
    vec2 screenUV = gl_FragCoord.xy * vec2(u.p2.y, u.p2.z);

    vec3 N = normalize(vNormal);
    vec3 V = normalize(u.camPos.xyz - vWorldPos);   // toward the camera
    vec3 sunDir = normalize(u.sunDir.xyz);

    // --- High-frequency ripple: perturb the normal with two cheap scrolling sine
    // gradients so the surface sparkles between the macro Gerstner waves. ---
    float time = u.p0.y;
    vec2 rp = vWorldPos.xz;
    float r1 = sin(rp.x * 1.7 + time * 1.3) + cos(rp.y * 1.9 - time * 1.1);
    float r2 = sin((rp.x + rp.y) * 2.3 - time * 1.7);
    vec3 ripple = vec3(0.06 * (cos(rp.x * 1.7 + time * 1.3) + r2 * 0.5),
                       0.0,
                       0.06 * (-sin(rp.y * 1.9 - time * 1.1) + r2 * 0.5));
    N = normalize(N + ripple);

    // --- Depth-based water color: how much water the view ray crosses before the
    // opaque scene floor. Compare the scene depth (behind water) to this
    // fragment's depth, both linearized to eye distance. ---
    float sceneD = texture(sceneDepth, screenUV).r;
    float sceneDist = linearizeDepth(sceneD);
    float surfDist  = linearizeDepth(gl_FragCoord.z);
    float waterDepth = max(sceneDist - surfDist, 0.0);
    // Shallow near 0 m of water, fully deep by ~6 m.
    float depthT = clamp(waterDepth / 6.0, 0.0, 1.0);
    vec3 refractCol = mix(u.shallowColor.rgb, u.deepColor.rgb, depthT);

    // --- Reflection: analytic sky in the mirror direction. ---
    vec3 R = reflect(-V, N);
    R.y = max(R.y, 0.02);              // never sample below the horizon (no black)
    vec3 reflectCol = skyColor(normalize(R), sunDir);

    // --- Fresnel (Schlick) blend: face-on -> refraction, grazing -> reflection. ---
    float base = clamp(u.p1.w, 0.0, 1.0);
    float fres = base + (1.0 - base) * pow(1.0 - max(dot(N, V), 0.0), 5.0);
    vec3 color = mix(refractCol, reflectCol, fres);

    // --- Sun glint: sharp Blinn-Phong-ish specular toward the sun (HDR; bloom). ---
    vec3 H = normalize(sunDir + V);
    float spec = pow(max(dot(N, H), 0.0), 220.0);
    color += vec3(1.0, 0.96, 0.88) * spec * u.p1.z;

    // A touch of diffuse sun on the body so deep water isn't flat/dead.
    float ndl = max(dot(N, sunDir), 0.0);
    color += refractCol * ndl * 0.15;

    // --- Horizon fog: blend the far water into the sky so the sea meets the sky
    // with no hard seam. Fades with view distance + as the patch reaches its edge. ---
    float viewDist = length(u.camPos.xyz - vWorldPos);
    float distFog = clamp((viewDist - 80.0) / 220.0, 0.0, 1.0);
    float edge    = max(abs(vGrid.x), abs(vGrid.y));
    float edgeFade = smoothstep(0.82, 1.0, edge);
    float fog = max(distFog, edgeFade);
    vec3 horizonSky = skyColor(normalize(vWorldPos - u.camPos.xyz + vec3(0.0, 0.0, 0.0)), sunDir);
    // Use a horizon-ish sky tint for the fade target (slightly toward the camera ray).
    color = mix(color, horizonSky, fog);

    outColor = vec4(color, 1.0);
}
