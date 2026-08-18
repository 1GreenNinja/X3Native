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
    vec2 basePos = center + inGrid * halfExt;  // flat world XZ before waves

    // ---- RIVER MODE (task #32): the surface level FOLLOWS the channel. ----
    // Closest approach to the node polyline (the CPU polyClosest, in GLSL):
    // the per-node waterY interpolated at the closest point is the local
    // level — the same answer worldWaterLevelAt gives, so the drawn plane
    // and the query are ONE truth. Outside halfWidth the surface fades out
    // (the waterline), except inside the ocean basin disc where the level
    // hands off to the sea instead (the estuary reaches open water).
    float mask = 1.0;
    int rn = int(u.riverInfo.x + 0.5);
    if (rn >= 2) {
        float hw = max(u.riverInfo.y, 1.0);
        float best = 1e30;
        float lvl  = u.riverNodes[0].z;
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
            }
        }
        float d = sqrt(best);
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
    const int   N = 4;
    vec2  dirs[4] = vec2[4](
        normalize(vec2( 1.0,  0.25)),
        normalize(vec2(-0.6,  0.8 )),
        normalize(vec2( 0.2, -1.0 )),
        normalize(vec2(-0.9, -0.35)));
    float lenMul[4] = float[4](1.0, 0.55, 0.32, 0.18);
    float ampMul[4] = float[4](1.0, 0.5,  0.28, 0.14);

    vec3 disp = vec3(0.0);
    vec3 dPdx = vec3(0.0);
    vec3 dPdz = vec3(0.0);
    for (int i = 0; i < N; ++i) {
        float L  = baseLen * lenMul[i];
        float w  = 6.28318530718 / L;          // 2*pi / wavelength
        float A  = amp * ampMul[i];
        // Deep-water dispersion: phase speed ~ sqrt(g/k); fold the user speed in.
        float phi = sqrt(9.81 * w) * speed;
        float Q  = steep / max(w * A * float(N), 1e-4);
        gerstner(dirs[i], w, A, Q, phi, time, basePos, disp, dPdx, dPdz);
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
    vCrest    = disp.y;
    gl_Position = u.viewProj * vec4(worldPos, 1.0);
}
