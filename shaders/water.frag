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
    // y = foam (WaterParams::foam): 0 = off (legacy, byte-identical); >0
    // scales CONTACT FOAM where the water thins onto banks/rocks/hulls (the
    // scene-depth trick below) + CREST FOAM where the Gerstner lift tops out.
    vec4  p4;
    // RIVER MODE (task #32) — consumed by the vertex stage (level-follow +
    // coverage mask); declared here so the block layouts match. In river mode
    // p0.x (seaLevel) carries the LOCAL level at the camera (the host feeds
    // worldWaterLevelAt(cam)), so the underside-view gate below stays honest.
    vec4  riverInfo;
    vec4  riverBasin;
    // Shoreline table (W-UNDERRIVER) — consumed by the vertex stage; declared
    // so the block layouts match. See water.vert.
    vec4  shoreInfo;
    vec4  shoreRadii[64];
    vec4  riverNodes[20];
    // ROOM LIGHTS (WaterParams::roomLight*): roomInfo.x = count (0 = none).
    // Two vec4 per light: [2i] = xyz position, w range; [2i+1] = rgb
    // colour*intensity. Only read on the enclosed path (p4.z > 0).
    vec4  roomInfo;
    vec4  roomLights[32];
    // FLOW / RAPIDS (app/river_rapids.h; see water.vert for the layout).
    vec4  flowInfo;
    vec4  flowLut[64];
    vec4  rocks[12];
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
// Raw Gerstner lift (m) — crest-foam driver (only read when u.p4.y > 0).
layout(location = 4) in float vCrest;
// FLOW (river-rapids; water.vert): xy = downstream tangent, z = speed (m/s,
// 0 = no flow -> every rapids term below is skipped, Rev 11 byte for byte),
// w = turbulence 0..1.
layout(location = 5) in vec4 vFlow;
// CHANNEL frame: x = along-chain s, y = lateral / half-width, z = standing-
// wave lift normalised (-1..1), w = standing-wave amplitude (m).
layout(location = 6) in vec4 vChan;

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

// THE SKY THIS WATER CAN ACTUALLY SEE. p4.z (WaterParams::enclosed) is 0 for
// open water — the analytic sky above, byte-for-byte the historic term. At 1
// the water is under a roof and there is no sky: the reflection and the fade
// both hand off to horizonColor (p3.rgb) instead. Without this a cave river
// mirrors a bright daylight sky and photographs as crumpled chrome foil.
vec3 skyOrRoof(vec3 dir, vec3 sunDir) {
    float enc = clamp(u.p4.z, 0.0, 1.0);
    if (enc <= 0.0) return skyColor(dir, sunDir);
    return mix(skyColor(dir, sunDir), u.p3.rgb, enc);
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

// Point-light falloff — the SAME window mesh.frag applies to the rock (inc/
// mesh_lighting.glsl pointAtten; that file pulls in the cluster buffers so it
// is mirrored here rather than included). Keep the two identical or the water
// and the bank it laps will disagree on how bright a lamp is.
float roomAtten(float dist, float range) {
    float t = dist / max(range, 0.0001);
    float w = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    w *= w;
    return w / (dist * dist + 1.0);
}

// ROOM LIGHTING for ENCLOSED water (p4.z > 0, roomInfo.x > 0). Everything the
// open-water path adds up is a RADIANCE under an implicit daylight irradiance
// of ~1 — shallowColor is what a sunlit shallow reads as, the reflection is a
// sky. Under a roof there is no sun and no sky, so those numbers became
// SELF-LUMINOUS: the canon cavern's river photographed as a flat glowing cyan
// slab ten times brighter than the rock beside it, unrelated to the 44 bank
// lights that light that rock. This computes what actually falls on the
// surface: a diffuse ambient of PI * horizonColor (the vault radiance the
// reflection already uses, integrated over the hemisphere) plus every handed-
// over room light with the rock's own falloff and Lambert term. The specular
// out-parameter is each light's GGX lobe on the rippled surface — the streaks
// of bank light on dark water that make a cave river read as WATER instead of
// as a painted ribbon (rougher than the sun glint: the ripple normal already
// carries the high-frequency sparkle, so a mirror lobe would strobe).
vec3 roomLighting(vec3 P, vec3 N, vec3 V, float fresBase, out vec3 specOut) {
    vec3 E = u.p3.rgb * 3.14159265;
    specOut = vec3(0.0);
    int ln = int(u.roomInfo.x + 0.5);
    const float kRough = 0.22;                 // perceptual roughness of the sheet
    const float a2 = (kRough * kRough) * (kRough * kRough);
    float NoV = max(dot(N, V), 1e-4);
    for (int i = 0; i < ln; ++i) {
        vec4 pr = u.roomLights[2 * i];
        vec3 lc = u.roomLights[2 * i + 1].rgb;
        vec3 toL = pr.xyz - P;
        float d = length(toL);
        vec3 L = toL / max(d, 1e-4);
        float NoL = max(dot(N, L), 0.0);
        if (NoL <= 0.0) continue;
        float att = roomAtten(d, pr.w);
        E += lc * att * NoL;
        vec3 H = normalize(L + V);
        float NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
        float dd = NoH * NoH * (a2 - 1.0) + 1.0;
        float D = a2 / (3.14159265 * dd * dd);
        float F = fresBase + (1.0 - fresBase) * pow(1.0 - VoH, 5.0);
        specOut += lc * att * (D * F / (4.0 * NoV)) * NoL;
    }
    return E;
}

// ---- THE FLOW MAP'S TWO PHASES (river-rapids). --------------------------
// A pattern carried downstream forever runs off the end of float precision
// and, worse, tears at every polyline bend (neighbouring segments carry it
// different ways, and the tear grows with time). The standard cure: two
// copies of the pattern, each advected only over a short cycle
// (kFlowCycle s) and reset, offset half a cycle from each other, and
// cross-faded so the copy that resets always has zero weight at the reset —
// no pop, and the offset is bounded by speed*kFlowCycle/2 metres. The speed
// is per second and the clock is the host's dt-scaled water clock, so the
// pattern moves speed*dt per frame at any frame rate.
// PAIRED with river_rapids.cpp riverFlowAdvect(): same cycle, same law.
const float kFlowCycle = 2.4;
void flowAdvect(float time, float speed, vec2 dir, out vec2 offA, out vec2 offB, out float wA) {
    float phA = fract(time / kFlowCycle);
    float phB = fract(phA + 0.5);
    offA = -dir * speed * (phA - 0.5) * kFlowCycle;
    offB = -dir * speed * (phB - 0.5) * kFlowCycle;
    wA   = 1.0 - abs(2.0 * phA - 1.0);
}

// The Rev 11 ripple normal at pattern coordinate q (world XZ, possibly
// advected) and pattern clock tt. Refactored out of main so the flow path can
// evaluate it twice; the legacy path calls it once with (rp, time) and gets
// the identical expression.
vec3 rippleN(vec2 q, float tt) {
    float r2 = sin((q.x + q.y) * 2.3 - tt * 1.7);
    return vec3(0.06 * (cos(q.x * 1.7 + tt * 1.3) + r2 * 0.5),
                0.0,
                0.06 * (-sin(q.y * 1.9 - tt * 1.1) + r2 * 0.5));
}

// The CHURN: the 2-4 m surface roughness a rapid has that the vertex grid
// cannot carry (river_rapids.cpp's Nyquist note). Higher-frequency gradients
// than the ripple, advected with the flow, amplitude by turbulence.
vec3 churnN(vec2 q, float tt, float turb) {
    float a = sin(q.x * 4.1 + q.y * 1.3 - tt * 2.9) * cos(q.y * 3.7 - q.x * 0.9 + tt * 2.1);
    float b = sin((q.x - q.y) * 5.3 + tt * 3.7);
    return vec3(0.22 * turb * (cos(q.x * 4.1 + q.y * 1.3 - tt * 2.9) + 0.6 * b),
                0.0,
                0.22 * turb * (-sin(q.y * 3.7 - q.x * 0.9 + tt * 2.1) * 0.9 + 0.6 * a));
}

// The STREAK ROUGHNESS: the skin of a rapid between the foam is not a
// sheet. It is drawn into fine ridges along the current, 10-40 cm apart,
// and the mirror it holds breaks along them. Without this the standing
// waves' faces — smooth at the vertex scale, and the churn above is 2-4 m —
// mirrored the sky opening as broad blue-white sheets that the eye read as
// foam (v3/v4 rapid_gorge_downstream: every "soft raft" was one). Slope
// mostly ACROSS the flow (the ridges run along it) and one across-flow
// frequency modulated slowly along it so the ridges braid. In the flow frame
// (along, across), unstretched metres; amplitude by turbulence, so a calm
// reach stays glass and the flow-off path never reaches it.
vec3 streakN(vec2 q, vec2 dir, vec2 per, float tt, float turb) {
    float c  = q.y, a = q.x;
    float s1 = sin(c * 14.0 + 0.8 * sin(c * 5.3 + a * 0.6 - tt * 1.1));
    float s2 = sin(c * 27.0 - 0.7 * sin(c * 9.1 - a * 0.4) + tt * 3.0);
    float across = 0.22 * turb * (s1 + 0.6 * s2);
    float along  = 0.08 * turb * sin(a * 2.0 + c * 3.0 - tt * 2.2);
    vec2 g = per * across + dir * along;
    return vec3(g.x, 0.0, g.y);
}

// ---- THE WHITEWATER. Each function below is ONE expression mirrored in
// river_rapids.cpp (riverWhitewaterMask / laceOne / riverWhitewaterThreshold /
// riverWhitewaterCover); gate R9 integrates them over a rapid's centre and
// demands 25-50% cover. Edit both sides or the gate goes red.
//
// wwMask — WHERE foam forms. The standing-wave crest caps (the bands across
// the channel; a full-turbulence cap reaches 1 = a solid broken-white band),
// the outer quarter of the width in fast water, the boulder piles and wakes;
// the 0.30*turb floor is the sparse lace on the dark fast water between.
// (v4: 0.40 / 0.55 with the bank term from 60% of the half-width spread
// the foam evenly over the whole surface and the crest bands drowned in it.)
float wwMask(float turb, float latN, float crest, float wake) {
    float bank = smoothstep(0.75, 1.00, abs(latN));
    float cap  = smoothstep(0.15, 0.85, crest);
    return clamp(0.30 * turb + 1.00 * cap * turb + 0.35 * bank * turb + wake, 0.0, 1.0);
}
// wwLaceOne — the foam pattern at flow-frame coordinate q = (along / 4,
// across): everything is stretched 4x along the current (foam in moving
// water is drawn into streamers), four octaves from 2 m patchiness (n1,
// which domain-warps the rest so no octave lines up) through 0.6 m cells
// (n2) to the 0.2 m lace (n3, faded by `fine` with view distance — it is
// shimmer past 40 m), plus st: 1-D noise ACROSS the flow, i.e. long streaks
// along it. Sums of sines, not textures: no seam, no tile, any scale.
float wwLaceOne(vec2 q, float fine) {
    float n1 = sin(q.x * 1.9 + q.y * 1.3) * cos(q.x * 1.1 - q.y * 2.4);
    q.y += 0.35 * n1;
    float n2 = sin(q.x * 7.3 + q.y * 9.1) * cos(q.x * 4.7 - q.y * 11.2)
             + 0.7 * sin(q.x * 5.1 - q.y * 14.3 + 1.7) * sin(q.x * 9.7 + q.y * 6.9);
    float n3 = sin(q.x * 23.0 + q.y * 31.0) * cos(q.x * 17.0 - q.y * 37.0)
             + 0.6 * sin(q.x * 29.0 - q.y * 19.0 + 0.7) * sin(q.x * 13.0 + q.y * 41.0);
    float st = sin(q.y * 6.3 + 0.9 * sin(q.y * 2.1 + q.x * 0.35))
             + 0.6 * sin(q.y * 17.0 + 1.2 * sin(q.y * 4.7 - q.x * 0.5));
    return 0.5 + 0.06 * n1 + 0.28 * n2 + 0.30 * fine * n3 + 0.14 * st;
}
const float kLaceStretch = 4.0;
// wwThreshold / wwCover — foam is binary at the bubble scale: lace over a
// threshold that falls with the mask (0.92 where nothing should form, 0.22
// under a full crest cap or at a bow pile — a solid raft with dark holes
// through it — a few percent of flecks on the water between), across a
// 0.07 edge: the line of the bubble raft, not a fade to milk. The steep
// fall is the point: the mask decides WHERE, and a place is foaming or it
// is not (the even 0.87 -> 0.34 of v3/v4 made every square metre half foam
// and the eye could find no crest, no wake and no dark water). The trailing
// smoothstep is "no mask, no foam": still water grows not one fleck however
// high the lace peaks.
float wwThreshold(float mask) { return 0.92 - 0.70 * mask; }
float wwCover(float mask, float lace) {
    float th = wwThreshold(mask);
    return smoothstep(th, th + 0.07, lace) * smoothstep(0.0, 0.05, mask);
}
// wwCoverBlend — the two advected phases, each THRESHOLDED, each faded hard
// about its own half-weight (0.4..0.6 of the 1.2 s ramp = ~0.25 s), the
// brighter kept. Blending the two lace fields before the threshold (v2)
// halved the pattern's contrast whenever both phases carried weight: every
// raft edge went soft and the 0.2 m lace vanished. A linear blend of the
// two covers (v3) drew the phase on its way out as a grey ghost of every
// raft. This way a raft forms and dissolves in a quarter second — which is
// what foam in a rapid does — and a max, not a sum, so a spot both phases
// cover is white once and the coverage does not pulse with the cycle.
float wwCoverBlend(float mask, float laceA, float laceB, float wA) {
    return max(wwCover(mask, laceA) * smoothstep(0.4, 0.6, wA),
               wwCover(mask, laceB) * smoothstep(0.4, 0.6, 1.0 - wA));
}

// Boulder wakes: for every rock in the UBO, the bow pile on the upstream
// face (the brightest thing in a rapid — it sits on the vertex-stage bump)
// and the V downstream: two foam STREAKS off the rock's shoulders that
// diverge as the wake widens, with a fainter churned core between them,
// fading over the wake length. In the rock's own frame (along = flow
// tangent, across = its perpendicular).
float rockWake(vec2 P, vec2 dir) {
    int rn = int(u.flowInfo.z + 0.5);
    vec2 per = vec2(-dir.y, dir.x);
    float wake = 0.0;
    for (int i = 0; i < rn; ++i) {
        vec4 r = u.rocks[i];
        vec2 dp = P - r.xy;
        float R = max(r.z, 0.3), L = max(r.w, 1.0);
        float along = dot(dp, dir), across = abs(dot(dp, per));
        // the pile: water stacking on the upstream face and sheeting off the sides
        float bow = (1.0 - smoothstep(R * 0.9, R * 1.9, length(dp))) * (1.0 - smoothstep(-R * 0.3, R * 1.0, along));
        // the wake: half-width grows from 0.75R at the rock to 1.3R at the
        // end — a V the width of the rock and a half, not the channel (v4's
        // 1.8R end put two wakes edge to edge across the whole run, and the
        // eye lost both — eyes-on rapid_boulders_wake). The two edge streaks
        // are solid (1), the churned core between them a 0.35 mask: half
        // lace, dark water showing through, so the V reads as two lines.
        float ta  = clamp(along / L, 0.0, 1.0);
        float hwk = R * (0.75 + 0.55 * ta);
        float edges = 1.0 - smoothstep(0.0, 0.35 * R, abs(across - hwk));
        float core  = 0.35 * (1.0 - smoothstep(hwk * 0.5, hwk, across));
        float fade  = 1.0 - smoothstep(0.25 * L, L, along);
        float w = (along > 0.0) ? max(edges, core) * fade : 0.0;
        wake = max(wake, max(bow, w));
    }
    return wake;
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
    // ---- THE FLOW (river-rapids). flowOn: this fragment is on a flowing
    // river. The ripple pattern is then CARRIED by the current (two-phase
    // advection, above) instead of scrolling in place, its own clock slowed
    // so what you see moving is the water; a rapid adds the churn normal on
    // top. rpA/rpB are the two advected pattern coordinates and wA their
    // blend — reused by the foam noise below so the foam rides the same water.
    bool  flowOn = (u.flowInfo.x > 0.5) && (vFlow.z > 0.0);
    float turb = flowOn ? clamp(vFlow.w, 0.0, 1.0) : 0.0;
    vec2  rpA = rp, rpB = rp;
    float wA = 1.0;
    vec3 ripple;
    if (flowOn) {
        vec2 oA, oB;
        flowAdvect(time, vFlow.z, normalize(vFlow.xy), oA, oB, wA);
        rpA = rp + oA; rpB = rp + oB;
        float tt = time * 0.35;
        ripple = mix(rippleN(rpB, tt), rippleN(rpA, tt), wA);
        if (turb > 0.0) {
            ripple += mix(churnN(rpB, tt, turb), churnN(rpA, tt, turb), wA);
            vec2 fd = normalize(vFlow.xy), fp = vec2(-fd.y, fd.x);
            vec2 qA = vec2(dot(rpA, fd), dot(rpA, fp)), qB = vec2(dot(rpB, fd), dot(rpB, fp));
            ripple += mix(streakN(qB, fd, fp, tt, turb), streakN(qA, fd, fp, tt, turb), wA);
        }
    } else {
        ripple = rippleN(rp, time);
    }
    N = normalize(N + ripple);

    // --- Depth-based water color: how much water the view ray crosses before the
    // opaque scene floor. Compare the scene depth (behind water) to this
    // fragment's depth, both linearized to eye distance. ---
    float sceneD = texture(sceneDepth, screenUV).r;
    float sceneDist = linearizeDepth(sceneD);
    float surfDist  = linearizeDepth(gl_FragCoord.z);
    float waterDepth = max(sceneDist - surfDist, 0.0);
    // BEER-LAMBERT EXTINCTION, per channel — not a two-colour lerp.
    //
    // This used to be mix(shallow, deep, depth/6), which is why the water read
    // flat: a linear blend between two authored colours CANNOT produce the way
    // real water loses its channels at wildly different rates. Red is gone in
    // about a metre, green survives ~10 m, blue ~30 m — that spread is exactly
    // what makes shallows turquoise and depth go blue-black, and it is the
    // whole look. Transmittance T = exp(-extinction * depth) per channel.
    //
    // Extinction is derived from the AUTHORED colours so every existing
    // WaterParams keeps its identity: deepColor is what survives at the
    // reference depth, so extinction = -ln(deepColor)/refDepth. shallowColor is
    // the in-scattered light the column adds back (UE calls this the scattering
    // term; SingleLayerWaterShading.ush builds ExtinctionCoeff the same way,
    // from scattering + absorption). clarity stretches the reference depth:
    // clear water carries its colour further before it extinguishes.
    // Per-metre extinction, RED FIRST. Clear water loses red in about a metre,
    // green in ~10 m, blue in ~30 m; that spread is the entire reason shallows
    // read turquoise and depth goes blue-black. clarity stretches the whole
    // curve (clear water carries its colour further before extinguishing).
    float clarityM  = mix(0.55, 2.4, clamp(u.p2.x, 0.0, 1.0));
    vec3  kExt      = vec3(0.46, 0.10, 0.045) / clarityM;
    vec3  T         = exp(-kExt * waterDepth);   // transmittance per channel
    // What survives from the lit shallow floor, plus what the column scatters
    // back. Reduces to shallowColor at the waterline and to deepColor far down,
    // so every authored WaterParams keeps its identity — but the journey between
    // them is now exponential and per-channel instead of one linear ramp.
    vec3 refractCol = T * u.shallowColor.rgb + (vec3(1.0) - T) * u.deepColor.rgb;
    const float kRefDepth = 6.0;
    float depthT = clamp(waterDepth / kRefDepth, 0.0, 1.0);

    // --- Reflection: analytic sky in the mirror direction. ---
    vec3 R = reflect(-V, N);
    R.y = max(R.y, 0.02);              // never sample below the horizon (no black)
    vec3 reflectCol = skyOrRoof(normalize(R), sunDir);

    // --- Fresnel (Schlick) blend: face-on -> refraction, grazing -> reflection. ---
    float base = clamp(u.p1.w, 0.0, 1.0);
    float fres = base + (1.0 - base) * pow(1.0 - max(dot(N, V), 0.0), 5.0);

    // --- ENCLOSED: light the body from the room instead of from an implied
    // sky (see roomLighting). roomIrr stays exactly 1 for open water and for
    // an enclosed set that hands over no lights, so both are byte-identical. ---
    float enc = clamp(u.p4.z, 0.0, 1.0);
    vec3 roomIrr  = vec3(1.0);
    vec3 roomSpec = vec3(0.0);
    if (enc > 0.0 && u.roomInfo.x > 0.5) {
        vec3 sp;
        vec3 E = roomLighting(vWorldPos, N, V, base, sp);
        roomIrr  = mix(vec3(1.0), E, enc);
        roomSpec = sp * enc;
        // THE LAMP SHEET COLLAPSES IN WHITEWATER (river-rapids). The lobe
        // above is a 0.22-rough mirror of each bank lamp; on a rapid's
        // standing-wave faces — smooth at the vertex scale — it painted
        // broad blue-white sheets that the eye read as foam (eyes-on v3/v4
        // rapid_gorge_downstream: they, not the lace, were the "soft rafts";
        // the sky mirror there is dark). A rapid's skin is broken below the
        // pixel: the coherent lobe is gone and its energy comes back as the
        // diffuse aeration and the foam, both already counted. Keep a fifth
        // for the gloss between the rafts (streakN breaks it into streaks).
        // (Widening the lobe instead — roughness 0.6 — blew the faces out
        // white: this GGX carries no Smith term and at grazing NoV the wide
        // lobe is unbounded. Not worth adding G for, and G would move the
        // calm river's bytes.) turb is 0 in calm water and with the flow
        // off: byte-identical there.
        roomSpec *= 1.0 - 0.80 * turb;
    }
    // AERATED WATER (river-rapids): a rapid's body is full of bubbles and
    // reads pale, opaque green-white, not clear — the same fraction the foam
    // mask leaves open. Scaled by turbulence; zero in calm water and with the
    // flow off (byte-identical there). Applied before the room's light so it
    // is lit like the rest of the body, not painted on.
    if (turb > 0.0) {
        vec3 aerated = mix(u.shallowColor.rgb, vec3(0.42, 0.55, 0.56), 0.55);
        refractCol = mix(refractCol, aerated, 0.22 * turb);
    }
    refractCol *= roomIrr;
    vec3 color = mix(refractCol, reflectCol, fres) + roomSpec;

    // --- Sun glint: sharp Blinn-Phong-ish specular toward the sun (HDR; bloom). ---
    vec3 H = normalize(sunDir + V);
    float spec = pow(max(dot(N, H), 0.0), 220.0);
    // The glint is a SUN glint; under a roof there is no sun to glint at.
    color += vec3(1.0, 0.96, 0.88) * spec * u.p1.z * (1.0 - clamp(u.p4.z, 0.0, 1.0));

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

    // ---- FOAM (u.p4.y — the owner: "alive.. pulsing... writhing.. foaming
    // if needed"). Two sources, both physical:
    //   * CONTACT foam — where the water column thins to nothing against the
    //     scene (bank waterlines, rocks breaking the surface, a hull, a
    //     swimmer): waterDepth from the SAME scene-depth reconstruction the
    //     refraction gradient uses, so foam hugs every intersection with no
    //     extra geometry knowledge.
    //   * CREST foam — the Gerstner lift near the top of its travel whips
    //     white (only meaningful at ocean amplitudes; a calm river's crests
    //     stay under the threshold and its foam lives at the banks).
    // Both writhe under two scrolling interference noises. Lit by sun height
    // (night foam is grey, not glowing — ACES law, this is NOT emissive).
    // p4.y == 0 skips everything: legacy worlds byte-identical.
    float foamAmt = 0.0;
    float foamRelief = 1.0;   // whitewater clump thickness (river-rapids); 1 = legacy
    if (u.p4.y > 0.0) {
        float contactF = 1.0 - smoothstep(0.04, 1.05, waterDepth);
        // 0.70 threshold: at 0.62 a calm river's every second swell whipped
        // white and the near field read semi-stormy (eyes-on 17_waterline).
        float crestF   = smoothstep(0.70, 1.0, vCrest / max(u.p0.z * 1.35, 1e-3));
        float n1 = sin(rp.x * 0.83 + time * 1.9) * cos(rp.y * 1.07 - time * 1.6);
        float n2 = sin((rp.x - rp.y) * 2.61 + time * 2.7)
                 * sin((rp.x + rp.y * 0.7) * 1.31 - time * 1.2);
        float writhe = clamp(0.50 + 0.30 * n1 + 0.24 * n2, 0.0, 1.0);
        foamAmt = clamp((contactF + crestF * 0.6) * writhe * u.p4.y, 0.0, 1.0);
        // ---- WHITEWATER (river-rapids). Only on a flowing river; the
        // legacy contact + crest lace above is untouched and this adds to it.
        // Mask = reach turbulence (river_rapids.cpp's LUT, ramped over the
        // reach edges) + standing-wave crests + the banks in fast water +
        // boulder bow piles and wakes, through one smoothstep — then broken
        // up by a second-octave noise ADVECTED with the flow (rpA/rpB/wA from
        // the ripple, so the foam travels with the water it is on) and
        // streaked along the current (foam in a rapid is drawn into lines
        // parallel to the flow, not blobs). Still albedo: foamCol * foamLit
        // below lights it by the room or the sun, never emissive.
        if (flowOn) {
            vec2 dir = normalize(vFlow.xy);
            vec2 per = vec2(-dir.y, dir.x);
            // In moving water the smooth contact film above gives way to the
            // lace: the shallow water over the bank shelves and the boulder
            // shoulders still foams (the shelf term joins the mask below)
            // but it foams as bubble rafts with the same edges and the same
            // flow stretch as the rest, not as a soft 2 m writhe. Eyes-on
            // v3/v4 of rapid_gorge_downstream: the whole near field was that
            // film rising and falling with the standing waves, and no
            // threshold in the lace could sharpen what it never touched.
            // Calm reaches (turb 0) keep the legacy film exactly — a pool's
            // waterline is still water.
            float moving = smoothstep(0.05, 0.40, turb);
            foamAmt *= 1.0 - moving;
            // The shelf: water under half a metre deep over the carved
            // bank shelves. Narrower than contactF's metre (the bed shelves
            // are 2.5 m wide each side, and at contactF's ramp the outer
            // third of the whole channel came up SOLID white and the run
            // read as a foam trough with a dark centre — eyes-on
            // rapid_boulders_wake, v4 debug). 0.55 puts the shelf at the
            // half-lace mask, a bright line only at the waterline itself.
            float shelf = (1.0 - smoothstep(0.04, 0.50, waterDepth)) * moving;
            // vChan.z is the crest-sharpened primary train, -1..1, from the
            // vertex stage (PAIRED riverStandingWaveCrest). Where the flow
            // is on but the LUT has no wave (a calm reach) it is 0: no cap.
            float wake  = rockWake(rp, dir) + 0.55 * shelf;
            float mask  = wwMask(turb, vChan.y, vChan.z, wake);
            // The pattern in the flow frame, two-phase advected (the same
            // rpA/rpB/wA the ripple rides, so the foam travels with the
            // water it is on). The 0.2 m lace octave fades out past 12-45 m:
            // beyond that it is one shimmering pixel and only aliases.
            float fine = 1.0 - smoothstep(12.0, 45.0, length(u.camPos.xyz - vWorldPos));
            vec2 qA = vec2(dot(rpA, dir) / kLaceStretch, dot(rpA, per));
            vec2 qB = vec2(dot(rpB, dir) / kLaceStretch, dot(rpB, per));
            float laceA = wwLaceOne(qA, fine), laceB = wwLaceOne(qB, fine);
            float cover = wwCoverBlend(mask, laceA, laceB, wA);
            // Foam is the cover, full white where it is (the mask chose the
            // place, the lace the shape — the amount is not a blend of the
            // two: a thin raft is still white). No film: the water between
            // the rafts stays dark and glossy (the lead read v1's 70% grey
            // as paint marbling).
            float ww    = cover;
            // relief: the body of a raft is not one white — bubble clusters
            // are thicker (brighter) where the lace runs high, thinner in
            // the veins between them, so the 0.2 m texture shows INSIDE the
            // raft too, not only on its edge. Per phase, like the cover.
            float th    = wwThreshold(mask);
            float rel   = mix(smoothstep(th, th + 0.30, laceB), smoothstep(th, th + 0.30, laceA), smoothstep(0.4, 0.6, wA));
            foamRelief = mix(1.0, 0.68 + 0.32 * rel, clamp(ww / max(foamAmt + ww, 1e-4), 0.0, 1.0));
            foamAmt = clamp(foamAmt + ww * u.p4.y, 0.0, 1.0);
        }
        // Enclosed foam is lit by the room, not by the sky overhead: the
        // same irradiance the body just received (white foam is albedo, not
        // a light source — under a bank lamp it is bright, between lamps it
        // is grey). An enclosed set with no room lights keeps the old flat
        // 0.42 lift.
        float foamLitOpen = 0.30 + 0.70 * max(sunDir.y, 0.0);
        vec3 foamLit = (u.roomInfo.x > 0.5)
            ? mix(vec3(foamLitOpen), roomIrr, enc)
            : vec3(mix(foamLitOpen, 0.42, enc));
        vec3 foamCol = vec3(0.82, 0.87, 0.90) * foamLit * foamRelief;
        color = mix(color, foamCol, foamAmt);
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
    vec3 horizonSky = skyOrRoof(normalize(vWorldPos - u.camPos.xyz + vec3(0.0, 0.0, 0.0)), sunDir);
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
    // SEE-THROUGH RIDES THE SAME EXTINCTION AS THE COLOUR. This used to be
    // (1.0 - depthT), a straight ramp that hit ZERO at kRefDepth — six metres —
    // so the surface went fully opaque there and a marine structure or a fish
    // any deeper than a swimming pool could never be seen THROUGH the water,
    // however clear it was. Worse, it was a second, unrelated depth curve: the
    // colour said one thing about how deep the light reached and the alpha said
    // another.
    //
    // Physically they are the same fact. Water that still transmits light still
    // transmits the image, so transparency is just the luminance of the
    // transmittance T we already computed. Clear water (high clarity -> low
    // extinction) now stays see-through for tens of metres and goes opaque
    // gradually, the way a reef flat does; murky water closes up fast. One
    // curve, one story, and "clear enough to watch fish go by" becomes a
    // property of the water instead of an impossibility.
    float transLum   = dot(T, vec3(0.2126, 0.7152, 0.0722));
    // ...but never ALL the way. At clarity 1 this went to ~0.98 transparent in
    // the shallows and the surface stopped reading as water at all — the bed
    // looked like wet grass with highlights on it. Real water always keeps a
    // presence: a few percent reflects at normal incidence and the column
    // always scatters something back. kMaxSeeThrough leaves that residue, so
    // the clearest water still tints and still has a surface — you see THROUGH
    // it rather than past it.
    const float kMaxSeeThrough = 0.86;
    float seeThrough = min(kMaxSeeThrough, u.p4.x * (1.0 - fres) * transLum);
    // Turbulent water is opaque (aerated body, above): the see-through dies
    // with turbulence. Zero effect in calm water and with the flow off.
    seeThrough *= (1.0 - 0.70 * turb);
    // Foam closes the surface back up (churned water is opaque white, and a
    // see-through foam patch would read as soap scum on glass).
    // Room-light streaks close the surface the way the sun glint does (a
    // highlight you can see the bed through is a highlight on the bed).
    float roomSpecLum = dot(roomSpec, vec3(0.2126, 0.7152, 0.0722));
    float alpha = clamp(1.0 - seeThrough + spec * u.p1.z * 0.25 + roomSpecLum * 4.0
                        + fog + foamAmt, 0.0, 1.0);
    outColor = vec4(color, alpha * vMask);
}
