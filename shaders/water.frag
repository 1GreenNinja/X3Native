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
    vec4  p2;   // x=patchHalfExtent, y=1/screenW, z=1/screenH, w=camera far plane (0 => legacy 200)
    // xyz = far-ocean handoff colour (linear); w = 1 when supplied, else 0.
    // See IRenderDevice::WaterParams::horizonColor — a world that draws its own
    // far-ocean mesh beyond this finite patch hands us the colour that mesh
    // renders as, so the patch edge dissolves into it instead of into the sky.
    vec4  p3;
    // x = clarity (WaterParams::clarity): 0 = the historic OPAQUE surface
    // (alpha 1 -> src*1+dst*0, byte-identical through the enabled blend);
    // >0 = face-on shallow water goes translucent over the lit scene beneath.
    vec4  p4;
    // RIVER MODE (task #32) — consumed by the vertex stage (level-follow +
    // coverage mask); declared here so the block layouts match. In river mode
    // p0.x (seaLevel) carries the LOCAL level at the camera (the host feeds
    // worldWaterLevelAt(cam)), so the underside-view gate below stays honest.
    vec4  riverInfo;
    vec4  riverBasin;
    vec4  riverNodes[20];
} u;

// Scene depth buffer (the SSAO depth pre-pass output). Sampled as data (R32F via
// the depth aspect) to recover the opaque-scene depth behind the water.
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vGrid;
// River-mode coverage (1 everywhere in legacy flat-sea mode — alpha * 1.0 is
// byte-identical). Fades to 0 across the channel waterline; <= 0 is dry land.
layout(location = 3) in float vMask;

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
// THE FAR PLANE IS LIVE (u.p2.w): the depth-based refraction gradient was
// silently wrong in every world that calls setCameraFar (the tunnel host runs
// far=4000; hardcoding 200 here made waterDepth garbage — the whole river read
// as one flat tint, the owner's "its only a surface level texture" verdict).
// p2.w == 0 keeps the historic 200 for any caller that predates the plumb.
const float kNear = 0.1;
float linearizeDepth(float d) {
    // d in [0,1], GLM_FORCE_DEPTH_ZERO_TO_ONE perspective.
    float kFar = (u.p2.w > 0.0) ? u.p2.w : 200.0;
    return (kNear * kFar) / (kFar - d * (kFar - kNear));
}

void main() {
    // River mode: past the channel's waterline there is no water — drop the
    // fragment before any shading. Legacy flat sea has vMask == 1 everywhere.
    if (vMask <= 0.004) discard;

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

    // --- UNDERSIDE VIEW (swimmer / riverbed camera below the plane). Seen from
    // below, dot(N,V) < 0 clamps to 0, Schlick goes to 1, and the surface
    // rendered as a PURE SKY MIRROR — the owner's "blinding white sheet" from
    // the dry riverbed. Physically the underside is the water column's own
    // absorbed light (dark green) with the bright refracted sun cone (Snell's
    // window) overhead. Cheap version: deep/shallow body color, a dimmed
    // sky term only near the up direction (the Snell window), glint killed.
    // Gated on camera-below-plane so every above-water frame is byte-identical.
    if (u.camPos.y < u.p0.x) {
        vec3 body = mix(u.deepColor.rgb, u.shallowColor.rgb, 0.30);
        // View ray from the camera to this fragment is -V; looking UP at the
        // surface means (-V).y > 0. (V itself points DOWN toward the camera.)
        float window = pow(max(-V.y, 0.0), 3.0);    // looking up -> brighter
        vec3 skyThrough = skyColor(vec3(0.0, 1.0, 0.0), sunDir) * 0.35;
        color = body * (0.45 + 0.55 * max(sunDir.y, 0.0)) + skyThrough * window;
        float depthFade = clamp(length(u.camPos.xyz - vWorldPos) / 60.0, 0.0, 1.0);
        color = mix(color, u.deepColor.rgb, depthFade);   // far water goes dark, not sky
        // Clarity from below: the Snell window overhead turns translucent so a
        // submerged swimmer sees light (and anything crossing the surface)
        // through it; the far, oblique surface stays a closed green lid.
        float aUnder = 1.0 - u.p4.x * 0.55 * window * (1.0 - depthFade);
        outColor = vec4(color, aUnder * vMask);
        return;
    }

    // --- Horizon fog: blend the far water into the sky so the sea meets the sky
    // with no hard seam. Fades with view distance + as the patch reaches its edge. ---
    float viewDist = length(u.camPos.xyz - vWorldPos);
    float distFog = clamp((viewDist - 80.0) / 220.0, 0.0, 1.0);
    float edge    = max(abs(vGrid.x), abs(vGrid.y));
    // EDGE FADE. The patch is a square centred on the camera, so `edge` is the
    // ONLY quantity that reaches 1.0 exactly where the geometry stops — the
    // distance fade cannot, because the edge sits 240 m away along the axes but
    // 339 m away at the corners. The old 0.82..1.0 band was 43 m of a 480 m
    // tile: at a sea-level grazing view that is a few screen pixels, i.e. a HARD
    // LINE, and along the axes the distance term had only reached 0.73 when the
    // geometry ended — a 27% colour step drawn dead-straight across the sea.
    // Widening the band to 0.35..0.995 spends ~155 m on the blend and guarantees
    // the patch is fully handed off BEFORE it terminates, in every direction.
    // Both the widened band and the handoff target are gated on p3.w, so a world
    // that supplies no horizonColor gets the historic 0.82..1.0 sky fade
    // BYTE-FOR-BYTE. Nothing outside Echo Harbor's sea changes.
    float edgeFade = mix(smoothstep(0.82, 1.0,   edge),
                         smoothstep(0.35, 0.995, edge), u.p3.w);
    float fog = max(distFog, edgeFade);
    vec3 horizonSky = skyColor(normalize(vWorldPos - u.camPos.xyz + vec3(0.0, 0.0, 0.0)), sunDir);
    // FADE TARGET. Historic behavior (p3.w == 0): the analytic sky, so the sea
    // melts into the sky at the horizon with no seam. When the host supplies a
    // far-ocean handoff colour (p3.w == 1) the EDGE fades into that instead, so
    // the patch dissolves into the mesh that continues the ocean past it; the
    // pure DISTANCE fade still targets the sky, so the true horizon is unchanged.
    vec3 edgeTarget = mix(horizonSky, u.p3.rgb, u.p3.w);
    float edgeShare = (fog > 1e-5) ? clamp(edgeFade / fog, 0.0, 1.0) : 0.0;
    vec3 fadeTarget = mix(horizonSky, edgeTarget, edgeShare);
    color = mix(color, fadeTarget, fog);

    // ---- CLARITY (u.p4.x): face-on SHALLOW water is translucent over the lit
    // scene beneath (the bed, the fish, a swimmer's body); opacity returns
    // with water depth (the same depthT the refraction gradient uses), at
    // grazing angles (fres -> the surface is a mirror there anyway), under the
    // sun glint (foam-bright pixels must not thin out), and into the horizon
    // fog. clarity 0 => alpha 1 everywhere — the historic opaque surface,
    // byte-identical through the enabled blend (src*1 + dst*0).
    float seeThrough = u.p4.x * (1.0 - fres) * (1.0 - depthT);
    float alpha = clamp(1.0 - seeThrough + spec * u.p1.z * 0.25 + fog, 0.0, 1.0);
    outColor = vec4(color, alpha * vMask);
}
