#version 450

// Animated water-surface vertex shader (undersea-world foundation).
//
// CLEAN-ROOM, original work. The wave model is the standard public
// Gerstner / trochoidal wave sum (sum-of-sines with horizontal pinch),
// implemented from public references (Tessendorf "Simulating Ocean Water",
// the Gerstner-wave chapter in GPU Gems, and public ocean-rendering articles).
// No game-engine source was consulted.
//
// GEOMETRY: a flat unit-patch grid mesh (vertex = its XZ position in [-1,1]) is
// uploaded once by the device. This VS expands the patch to a large world-space
// tile CENTERED under the camera (so the player is always near the dense middle
// of the grid) and SNAPPED to a coarse world grid each frame so the tessellation
// doesn't visibly swim under camera motion. The plane sits at the sea level y.
//
// WAVES: a small fixed set of Gerstner waves of differing direction / wavelength
// / amplitude is summed to displace each grid point (XZ pinch + Y lift) and to
// build an ANALYTIC normal (the exact partial derivatives of the same sum), so no
// normal map is needed for the macro shape. The fragment stage adds a cheap
// high-frequency ripple on top. Phase scrolls with water.time.

layout(set = 0, binding = 0) uniform WaterUBO {
    mat4  viewProj;     // camera view*proj (same matrix the meshes use)
    vec4  camPos;       // xyz = camera world position
    vec4  sunDir;       // xyz = normalized direction toward the sun
    vec4  deepColor;    // rgb = linear deep-water tint
    vec4  shallowColor; // rgb = linear shallow tint
    // x = seaLevel, y = time (s), z = amplitude (m), w = steepness (0..1)
    vec4  p0;
    // x = baseWavelength (m), y = speed, z = specular, w = fresnelBase
    vec4  p1;
    // x = patchHalfExtent (m), y = 1/screenW, z = 1/screenH, w = camera far
    vec4  p2;
    // xyz = far-ocean handoff colour (linear); w = 1 when supplied, else 0
    // (0 keeps the historic analytic-sky fade -- see IRenderDevice WaterParams)
    vec4  p3;
    // x = clarity (0 = legacy opaque; fragment-stage alpha) — declared here so
    // the block layout matches the fragment stage's.
    vec4  p4;
    // RIVER MODE (task #32 — one water truth). x = riverNodeCount (0 = the
    // historic flat sea, byte-identical), y = riverHalfWidth (m).
    vec4  riverInfo;
    // xy = ocean basin centre (world XZ), z = basin radius (0 = no sea
    // fallback), w = oceanLevel (sea surface Y the estuary hands off to).
    vec4  riverBasin;
    // THE SHORELINE TABLE (W-UNDERRIVER — water only IN bodies of water).
    // x = sector count (0 = legacy: the whole basin disc draws water, even
    // under the dry beach ring — the owner's "water underground"), y = fade
    // width (m). Radii below are the outermost radius per angular sector at
    // which the terrain bowl is actually below oceanLevel — computed on the
    // CPU from the SAME terrainHeightAtWorld the water query uses
    // (app/terrain.cpp worldOceanShoreTable — PAIRED, a change to one IS a
    // change to both). Past that radius there is beach/hill, not sea, and
    // the surface fades out exactly like the river's waterline.
    vec4  shoreInfo;
    vec4  shoreRadii[64];   // 256 radii, 4 per vec4, sector i at [i>>2][i&3]
    // Per node: x = world x, y = world z, z = waterY. The SAME node table the
    // terrain carve and worldWaterLevelAt interpolate — the drawn plane can
    // no longer stand above the carved water table downstream.
    vec4  riverNodes[20];
    // Room lights (enclosed water) — consumed by the fragment stage;
    // declared so the block layouts match. See water.frag.
    vec4  roomInfo;
    vec4  roomLights[32];
    // FLOW / RAPIDS (app/river_rapids.h). flowInfo.x = LUT sample count
    // (0 = off: nothing below is read and the pass is Rev 11 byte for byte),
    // .y = polyline length (m), .z = rock count. flowLut[k] = (speed m/s,
    // turbulence 0..1, standing-wave amplitude m, wavelength m) at
    // s = k/(count-1)*length. riverNodes[i].w = cumulative s at node i.
    vec4  flowInfo;
    vec4  flowLut[64];
    vec4  rocks[12];
} u;

layout(location = 0) in vec2 inGrid;   // patch coord in [-1,1]

layout(location = 0) out vec3 vWorldPos;   // displaced world position
layout(location = 1) out vec3 vNormal;     // analytic surface normal
layout(location = 2) out vec2 vGrid;       // [-1,1] patch coord (edge fade)
// River-mode coverage: 1 = water here, fading to 0 across the channel's
// waterline (fragment multiplies alpha; <= 0 discards). Always 1 in legacy
// flat-sea mode so every existing world renders byte-identically.
layout(location = 3) out float vMask;
// Raw Gerstner lift (m) of this vertex — the fragment stage turns lift near
// the top of its travel into crest foam (WaterParams::foam; 0 = off, and the
// varying is simply ignored, so legacy worlds are untouched).
layout(location = 4) out float vCrest;
// FLOW (river-rapids): xy = downstream unit tangent of the containing
// segment, z = speed (m/s), w = turbulence 0..1. All zero when the flow is off
// or outside river mode; the fragment stage gates on z > 0.
layout(location = 5) out vec4 vFlow;
// CHANNEL frame: x = along-chain s (m), y = signed lateral / half-width
// (-1..1 across the wet channel, beyond at the banks), z = standing-wave lift
// normalised to its amplitude (-1..1; crest foam), w = standing-wave amplitude
// at this vertex (m).
layout(location = 6) out vec4 vChan;

// One Gerstner wave: displaces a flat point p (XZ) and accumulates the analytic
// partial derivatives needed to build the surface normal. Direction d is a unit
// XZ vector; w = spatial frequency; phi = phase speed; A = amplitude; Q = the
// per-wave steepness (horizontal pinch). See the GPU Gems Gerstner formulation.
void gerstner(vec2 d, float w, float A, float Q, float phi, float t,
              vec2 basePos, inout vec3 disp, inout vec3 dPdx, inout vec3 dPdz) {
    float dotd = dot(d, basePos);
    float ph   = w * dotd + phi * t;
    float c    = cos(ph);
    float s    = sin(ph);
    float WA   = w * A;
    // Horizontal pinch toward the crest (Q) + vertical lift.
    disp.x += Q * A * d.x * c;
    disp.z += Q * A * d.y * c;
    disp.y += A * s;
    // Partial derivatives of the displaced position wrt the base x and z.
    // d(disp)/dx
    dPdx.x += -Q * WA * d.x * d.x * s;
    dPdx.z += -Q * WA * d.x * d.y * s;
    dPdx.y +=      WA * d.x * c;
    // d(disp)/dz
    dPdz.x += -Q * WA * d.x * d.y * s;
    dPdz.z += -Q * WA * d.y * d.y * s;
    dPdz.y +=      WA * d.y * c;
}

void main() {
    float seaLevel = u.p0.x;
    float time     = u.p0.y;
    float amp      = u.p0.z;
    float steep    = clamp(u.p0.w, 0.0, 1.0);
    float baseLen  = max(u.p1.x, 0.5);
    float speed    = u.p1.y;
    float halfExt  = max(u.p2.x, 1.0);

    // Expand the unit patch to a world tile centered under the camera, snapped to
    // a coarse cell so the grid doesn't crawl as the camera moves.
    float snap = (2.0 * halfExt) / 64.0;       // ~one cell of the coarse grid
    vec2 center = floor(u.camPos.xz / snap) * snap;
    // CAMERA-CENTRIC GRID WARP (river-rapids). The uniform 192-vertex grid is
    // 2.5 m cells, and a 3 m standing wave on 2.5 m cells is a rumour: the
    // lead read the v1 crests as a swell. With the flow field on, the unit
    // grid is remapped g = inGrid * (0.3 + 0.7*|inGrid|) per axis: the patch
    // still spans exactly [-1, 1] (the edge fade, mask and horizon handoff
    // see the same rim), but the cells near the camera — where the waves are
    // judged from eye height — are 0.75 m and the far ones 4.25 m, and a
    // 3 m wave has four vertices per period for ~60 m around the eye. The
    // vertices no longer sit on fixed world positions across a snap step
    // (the warp is camera-relative), so a very long wave would sample
    // slightly differently after each 7.5 m move; at these amplitudes it is
    // sub-pixel. Flow OFF keeps the legacy uniform grid byte-for-byte.
    vec2 g = (u.flowInfo.x > 0.5) ? inGrid * (0.3 + 0.7 * abs(inGrid)) : inGrid;
    vec2 basePos = center + g * halfExt;       // flat world XZ before waves

    // ---- RIVER MODE (task #32): the surface level FOLLOWS the channel. ----
    // Closest approach to the node polyline (the CPU polyClosest, in GLSL):
    // the per-node waterY interpolated at the closest point is the local
    // level — the same answer worldWaterLevelAt gives, so the drawn plane
    // and the query are ONE truth. Outside halfWidth the surface fades out
    // (the waterline), except inside the ocean basin disc where the level
    // hands off to the sea instead (the estuary reaches open water).
    float mask = 1.0;
    vec4 flow = vec4(0.0);
    vec4 chan = vec4(0.0);
    // Standing-wave displacement and its slope, accumulated in the channel's
    // (along, lateral) frame and rotated into the world tangent basis below.
    float swY = 0.0, swDs = 0.0, swDl = 0.0;
    float swX = 0.0, swXDs = 0.0;   // along-flow Gerstner pinch and its d/ds
    int rn = int(u.riverInfo.x + 0.5);
    if (rn >= 2) {
        float hw = max(u.riverInfo.y, 1.0);
        float best = 1e30;
        float lvl  = u.riverNodes[0].z;
        int   bi = 0;          // the containing segment (the closest one)
        float bt = 0.0;        // ... and the parameter along it
        float bside = 0.0;     // which side of the spine (sign of the 2-D cross)
        for (int i = 0; i + 1 < rn; ++i) {
            vec2  a  = u.riverNodes[i].xy;
            vec2  b  = u.riverNodes[i + 1].xy;
            vec2  ab = b - a;
            float l2 = max(dot(ab, ab), 1e-6);
            float t  = clamp(dot(basePos - a, ab) / l2, 0.0, 1.0);
            vec2  dp = basePos - (a + ab * t);
            float d2 = dot(dp, dp);
            if (d2 < best) {
                best = d2;
                lvl  = mix(u.riverNodes[i].z, u.riverNodes[i + 1].z, t);
                bi = i; bt = t;
                bside = sign(ab.x * dp.y - ab.y * dp.x);
            }
        }
        float d = sqrt(best);
        // ---- THE FLOW FIELD (river-rapids). The SAME closest-segment hit
        // that fixed the water level fixes the along-chain s and the lateral
        // offset, so flow, level and channel mask cannot disagree. Speed,
        // turbulence and the standing-wave train come from the 1-D LUT.
        if (u.flowInfo.x > 0.5) {
            vec2  a  = u.riverNodes[bi].xy;
            vec2  b  = u.riverNodes[bi + 1].xy;
            vec2  dir = normalize(b - a);
            float s   = mix(u.riverNodes[bi].w, u.riverNodes[bi + 1].w, bt);
            float lat = d * bside;                     // signed metres off the spine
            float fn  = u.flowInfo.x;
            float fu  = clamp(s / max(u.flowInfo.y, 1.0), 0.0, 1.0) * (fn - 1.0);
            int   k0  = int(floor(fu));
            int   k1  = min(k0 + 1, int(fn) - 1);
            vec4  f   = mix(u.flowLut[k0], u.flowLut[k1], fu - float(k0));
            float spd = f.x, turb = f.y, swA = f.z, swL = max(f.w, 2.0);
            // Fast water hugs the channel: the current dies against the banks
            // (no-slip), the standing waves die with it.
            float prof = 1.0 - smoothstep(0.55, 1.0, abs(lat) / hw);
            float ln  = lat / hw;
            flow = vec4(dir, spd * (0.35 + 0.65 * prof), turb);
            // STANDING WAVES. In a rapid the surface is a train of crests
            // fixed to the bed (they stand; the water runs through them):
            // wavelength from the Froude relation lambda = 2*pi*v^2/g,
            // floored at the water grid's Nyquist (river_rapids.cpp), crests
            // across the flow, curved into haystacks by the lateral term,
            // breathing slowly in time. A second train at ~half wavelength
            // gives the surge between crests, and a third, slower and
            // advected at the flow speed, is the boils that ride downstream.
            // Amplitude = LUT (turbulence * speed) * bank profile.
            vec2 per = vec2(-dir.y, dir.x);            // d(lat)/d(basePos)
            if (swA > 0.0005) {
                float A   = swA * prof;
                float k   = 6.28318530718 / swL;
                // PRIMARY TRAIN, crest-sharpened: h = (1+sin)/2 through
                // pow 2.2 — narrow steep crests, broad flat troughs, the
                // haystack profile (a plain sine read as a swell from eye
                // height). PAIRED with river_rapids.cpp
                // riverStandingWaveCrest(): the fragment stage gets f in
                // chan.z and caps the crests with foam where f > 0.15.
                float ph1 = k * s + 0.5 * ln * ln + 0.35 * sin(time * 1.7 + s * 0.05);
                float h   = max(0.5 + 0.5 * sin(ph1), 0.0);
                float f   = 2.0 * pow(h, 2.2) - 1.0;
                float df  = 2.2 * pow(h, 1.2) * cos(ph1);   // df/dph1
                // the surge between crests and the boils riding downstream
                float ph2 = k * 1.9 * s + 0.4 * ln - time * 0.8;
                float ph3 = 2.1 * s - time * spd * 2.1 + 1.3 * ln;
                float w3  = 0.7 * ln + time * 0.9;
                float e3  = cos(w3);
                float y1 = A * f, y2 = 0.30 * A * sin(ph2), y3 = 0.20 * A * sin(ph3) * e3;
                swY  = y1 + y2 + y3;
                // d/ds and d/dlat (analytic), for the normal
                swDs = A * df * k + 0.30 * A * k * 1.9 * cos(ph2) + 0.20 * A * 2.1 * cos(ph3) * e3;
                swDl = (A * df * 1.0 * ln + 0.30 * A * cos(ph2) * 0.4
                        + 0.20 * A * (cos(ph3) * 1.3 * e3 - sin(ph3) * 0.7 * sin(w3))) / hw;
                // GERSTNER PINCH along the flow: the crests gather water
                // toward themselves (Q = 0.5), so they stand up as ridges
                // rather than a heaved sheet. Same construction as
                // gerstner() above, in the channel frame.
                float Q = 0.5;
                float px = Q * A * cos(ph1);                 // along-flow shift
                float dpx = -Q * A * k * sin(ph1);           // d(px)/ds
                swX = px; swXDs = dpx;
                chan = vec4(s, ln, f, A);
            } else {
                chan = vec4(s, ln, 0.0, 0.0);
            }
            // BOW PILES. Water hitting a boulder stacks up on its upstream
            // face: a Gaussian mound just ahead of each rock, 0.18 R high at
            // full speed, with its analytic slope. The fragment stage puts
            // the foam pile on the same spot (rockWake), so the bright pile
            // sits on a real bump, not on flat water.
            int rkn = int(u.flowInfo.z + 0.5);
            float spdN = clamp(spd / 1.5, 0.0, 1.0);
            for (int i = 0; i < rkn; ++i) {
                vec4 r = u.rocks[i];
                vec2 dp = basePos - r.xy;
                float R = max(r.z, 0.3);
                if (dot(dp, dp) > 16.0 * R * R) continue;
                float along = dot(dp, dir), across = dot(dp, per);
                float ua = (along + 0.9 * R) / (0.8 * R), uc = across / (1.1 * R);
                float bump = 0.18 * R * spdN * exp(-ua * ua) * exp(-uc * uc);
                swY  += bump;
                swDs += bump * (-2.0 * ua / (0.8 * R));
                swDl += bump * (-2.0 * uc / (1.1 * R));
            }
        }
        vec2  bp = basePos - u.riverBasin.xy;
        bool  inBasin = (u.riverBasin.z > 0.0) &&
                        (dot(bp, bp) < u.riverBasin.z * u.riverBasin.z);
        // The channel's waterline: fade out across the last 2 m INSIDE the
        // ribbon edge, hitting zero exactly at halfWidth — the same boundary
        // worldWaterLevelAt flips to dry at, so the drawn skirt can never
        // outrun the model (the banks cross the level between ~27 and ~34 m
        // out, so this edge is normally already clipped underground by the
        // depth test; noclip under the bank sees nothing either way now).
        float channelMask = 1.0 - smoothstep(hw - 2.0, hw, d);
        if (inBasin) {
            // Estuary handoff: river level (rides ~0.1 proud) feathers into
            // the sea surface past the channel edge; the basin disc is sea —
            // terrain above oceanLevel clips it into the shoreline FROM
            // ABOVE, and the shoreline table bounds it from below (the disc
            // used to draw the full 950 m even under the dry beach ring and
            // the rim hills: a sheet of underground water round the coast).
            seaLevel = mix(lvl, u.riverBasin.w,
                           smoothstep(hw - 6.0, hw + 14.0, d));
            float shoreMask = 1.0;
            float ns = u.shoreInfo.x;
            if (ns >= 4.0) {
                // NEAREST sector, not a lerp: each sector stores the MAX
                // shoreline radius over exactly its own angular span
                // (worldOceanShoreTable supersamples it), so nearest-lookup
                // can never cut real sea off — a lerp across a radial reach
                // of coast cut 16 m of water at bearing 309.5 (RB12's
                // measurement). The residual is a <= one-arc-width staircase
                // of OVERDRAW that dies under the beach sand.
                float db  = length(bp);
                float ang = atan(bp.y, bp.x);                    // [-pi, pi]
                float fs  = (ang * 0.15915494309 + 0.5) * ns;    // sector coord
                int   ia  = int(mod(floor(fs + 0.5), ns));
                float rs  = u.shoreRadii[ia >> 2][ia & 3];
                float fade = max(u.shoreInfo.y, 1.0);
                shoreMask = 1.0 - smoothstep(rs - fade, rs, db);
            }
            mask = max(channelMask, shoreMask);
        } else {
            seaLevel = lvl;
            mask = channelMask;
        }
    }

    // --- Sum a few Gerstner waves (varied dir/length/amp). The largest wave uses
    // baseLen; successive waves shorten + steepen for chop. Each wave's per-crest
    // steepness Q is normalized by (w*A*numWaves) so the surface never loops. ---
    // NO TILING. The old set was four hand-picked directions with wavelength
    // ratios (1, 0.55, 0.32, 0.18). Two of those directions — (1, 0.25) and
    // (-0.9, -0.35) — are very nearly OPPOSED, and a pair of counter-running
    // trains of similar wavelength is a standing wave: it pins crests to fixed
    // world positions and lays a lattice over the surface. Ratios that share
    // factors then make the whole sum repeat on a short period. Together those
    // read, at distance, as an obvious repeating grid — the owner saw it
    // immediately in the first rig capture.
    //
    // Fix, and it is all in the choice of numbers:
    //   * DIRECTIONS on the GOLDEN ANGLE (137.5 deg). Successive trains are
    //     never parallel and never opposed, and the set never closes on itself,
    //     so no standing-wave lattice can form.
    //   * WAVELENGTHS in powers of 1/phi, which is irrational — the beat period
    //     of the sum is effectively unbounded rather than a few tiles wide.
    //   * A per-wave spatial OFFSET so the trains do not all share a phase
    //     origin at the world centre.
    //   * SIX waves instead of four: more incommensurate terms, longer beat,
    //     finer chop. The Q normalisation below divides by N, so steepness per
    //     crest stays bounded and the surface still cannot loop.
    const int   N = 6;
    const float kGoldenAng = 2.39996323;   // 137.5 deg, in radians
    const float kInvPhi    = 0.61803399;   // 1/phi — irrational by construction
    // Amplitudes fall off with wavelength and are normalised so the total lift
    // matches the old four-wave sum (1.92) — the sea keeps its authored scale.
    const float kAmpNorm   = 1.92 / 2.4708;

    vec3 disp = vec3(0.0);
    vec3 dPdx = vec3(0.0);
    vec3 dPdz = vec3(0.0);
    for (int i = 0; i < N; ++i) {
        float fi  = float(i);
        float ang = fi * kGoldenAng + 0.7;          // 0.7 = fixed seed rotation
        vec2  d   = vec2(cos(ang), sin(ang));
        float mul = pow(kInvPhi, fi);               // irrational length ladder
        float L   = baseLen * mul;
        float w   = 6.28318530718 / L;              // 2*pi / wavelength
        float A   = amp * mul * kAmpNorm;
        // Deep-water dispersion: phase speed ~ sqrt(g/k); fold the user speed in.
        float phi = sqrt(9.81 * w) * speed;
        float Q   = steep / max(w * A * float(N), 1e-4);
        // Per-wave phase origin, so the trains do not all start together.
        vec2  off = vec2(fi * 37.13, fi * -21.71);
        gerstner(d, w, A, Q, phi, time, basePos + off, disp, dPdx, dPdz);
    }

    // Standing waves ride on top of the Gerstner swell: lift, plus the slope
    // rotated from the channel frame (along = dir, lateral = its left
    // perpendicular) into d/dx and d/dz. Zero unless the flow is on.
    if (swY != 0.0 || swX != 0.0) {
        vec2 dir = flow.xy;
        vec2 per = vec2(-dir.y, dir.x);
        disp.y += swY;
        dPdx.y += swDs * dir.x + swDl * per.x;
        dPdz.y += swDs * dir.y + swDl * per.y;
        // the pinch: a horizontal shift along dir, varying along dir only
        disp.x += swX * dir.x;
        disp.z += swX * dir.y;
        dPdx.x += swXDs * dir.x * dir.x;
        dPdx.z += swXDs * dir.y * dir.x;
        dPdz.x += swXDs * dir.x * dir.y;
        dPdz.z += swXDs * dir.y * dir.y;
    }

    vec3 worldPos = vec3(basePos.x + disp.x, seaLevel + disp.y, basePos.y + disp.z);

    // Tangent basis: the columns are d(worldPos)/d(basePos.x) and /d(basePos.z).
    vec3 tx = vec3(1.0 + dPdx.x, dPdx.y, dPdx.z);
    vec3 tz = vec3(dPdz.x, dPdz.y, 1.0 + dPdz.z);
    vec3 nrm = normalize(cross(tz, tx));       // +Y up for a flat sea
    if (nrm.y < 0.0) nrm = -nrm;

    vWorldPos = worldPos;
    vNormal   = nrm;
    vGrid     = inGrid;
    vMask     = mask;
    vCrest    = disp.y - swY;   // the Gerstner lift alone (legacy crest foam)
    vFlow     = flow;
    vChan     = chan;
    gl_Position = u.viewProj * vec4(worldPos, 1.0);
}
