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
        // Enclosed foam is lit by the room, not by the sky overhead: the
        // same irradiance the body just received (white foam is albedo, not
        // a light source — under a bank lamp it is bright, between lamps it
        // is grey). An enclosed set with no room lights keeps the old flat
        // 0.42 lift.
        float foamLitOpen = 0.30 + 0.70 * max(sunDir.y, 0.0);
        vec3 foamLit = (u.roomInfo.x > 0.5)
            ? mix(vec3(foamLitOpen), roomIrr, enc)
            : vec3(mix(foamLitOpen, 0.42, enc));
        vec3 foamCol = vec3(0.82, 0.87, 0.90) * foamLit;
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
    // Foam closes the surface back up (churned water is opaque white, and a
    // see-through foam patch would read as soap scum on glass).
    // Room-light streaks close the surface the way the sun glint does (a
    // highlight you can see the bed through is a highlight on the bed).
    float roomSpecLum = dot(roomSpec, vec3(0.2126, 0.7152, 0.0722));
    float alpha = clamp(1.0 - seeThrough + spec * u.p1.z * 0.25 + roomSpecLum * 4.0
                        + fog + foamAmt, 0.0, 1.0);
    outColor = vec4(color, alpha * vMask);
}
