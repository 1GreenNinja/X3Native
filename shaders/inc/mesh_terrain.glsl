#ifndef X3_MESH_TERRAIN_GLSL
#define X3_MESH_TERRAIN_GLSL
// ===========================================================================
// Terrain material splat (open-world ground). Procedural height + slope blend of
// four tiling detail textures (grass / rock / snow / sand) keyed off the
// fragment's WORLD position + WORLD surface normal, with a little value noise to
// break the tiling. Clean-room: built from the public height/slope-splat +
// triplanar mapping technique (GPU Gems terrain, the standard height/slope splat
// articles) — no engine source consulted. Only runs when vTerrainFlag != 0; all
// other meshes skip this entirely and shade exactly as before.
//
// CONSTANTS (tunables): these mirror the host world config — kSeaLevel matches
// `--world ocean`'s seaLevel (14 m) and the band heights are fractions of the
// default heightScale (55 m). The sand band sits at the shoreline so it meets the
// ocean cleanly; grass is low+flat; rock is steep slope; snow caps high peaks.
// ---------------------------------------------------------------------------
// NOTE (W8-3) recalibrated to the CANONICAL WORLD FIELD (terrain.cpp
// worldFeatures): base rolling field 0..55 m with macro relief, plus 4 mountain
// ranges 7-10 km out whose peaks reach ~400-500 m. Bands are absolute world Y:
//   * SAND is a SHORELINE material only — gated by proximity to the offshore
//     ocean basin (kShoreXZ, matches terrain.cpp kBasinCx/Cz) so inland lows and
//     the flattened city/facility pads don't read as beach.
//   * SNOW lives on the high ranges only (kSnowBottom 180 — the base field can
//     never reach it) and prefers flatter ground (it slides off cliffs).
//   * ALPINE band: grass gives way to rock with altitude well below the snowline.
//   * Slope-rock thresholds unchanged — on real mountainsides normal.y drops far
//     below 0.90, so slopes saturate to full rock naturally.
//   * Range-theme tints (kVolcanoXZ / kMesaXZ / kCrystalXZ, matched to terrain.cpp
//     kRanges band midpoints): E volcanic = dark basalt rock + no snow; S mesa =
//     warm sandstone rock; W highlands = mossier grass.
const float kSeaLevel    = 4.0;    // world Y of the basin water surface (ocean_base)
const float kSandTop     = 16.0;   // beach fades out by here (near the basin only)
const float kSnowBottom  = 180.0;  // snow begins (mountain shoulders)
const float kSnowFull    = 265.0;  // fully snow by here (high peaks)
const float kAlpineLo    = 75.0;   // grass starts yielding to rock
const float kAlpineHi    = 140.0;  // fully rock by here (below the snow line)
// SLOPE-ROCK THRESHOLDS. These were 0.90/0.965, which is far too flat to mean
// "cliff": normal.y = 0.93 is a 21.5 deg grassy hillside, and the old curve made
// that 58% rock. On the rolling base field every gentle undulation therefore
// picked up a grey patch, which is exactly the "rock blobs scattered over a flat
// green field" 04_saddle showed. Real ground holds soil and grass to ~30-35 deg
// and only goes to bare rock past that, so the band now starts at 0.82
// (~35 deg) and is fully clear of rock by 0.94 (~20 deg).
const float kSlopeRockLo = 0.82;   // normal.y at/below this -> full rock (steep)
const float kSlopeRockHi = 0.94;   // normal.y at/above this -> no rock (flat)
const float kDetailScale = 0.18;   // world-space detail tiling (cycles / meter)
// Stochastic (hex-lattice) tiling — see stochXZ() below. Lattice period is in
// UV/cycle units: 0.35 puts a lattice cell at ~2.8 detail tiles (~16 m), so the
// random offsets decorrelate the repeat at the macro scale the eye picks up.
const float kStochLattice  = 0.35;
const float kStochContrast = 4.0;   // weight sharpening; 1 = plain linear blend
const float kMacroScale  = 0.012;  // large-scale tint variation frequency
const vec2  kShoreXZ     = vec2( 1100.0, -1350.0);  // ocean basin center
const vec2  kVolcanoXZ   = vec2( 9200.0,   250.0);  // E range band midpoint
const vec2  kMesaXZ      = vec2(  350.0, -9000.0);  // S range band midpoint
const vec2  kCrystalXZ   = vec2(-8600.0,  -100.0);  // W range band midpoint

// Cheap hash-based value noise on world XZ (self-contained; matches the CPU
// terrain's value-noise idea so the breakup reads consistent). Returns [0,1).
float thash(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}
float tnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = thash(i);
    float b = thash(i + vec2(1.0, 0.0));
    float c = thash(i + vec2(0.0, 1.0));
    float d = thash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// --- STOCHASTIC (HEX-LATTICE) TILING ----------------------------------------
// A tiling detail texture at kDetailScale repeats every 1/0.18 = 5.56 m. Over a
// cut face or an open hillside that repeat is plainly visible as a grid marching
// up the slope (08_exit_portal) and as identical blobs scattered over a field
// (04_saddle). This breaks it WITHOUT a second asset.
//
// Technique (Heitz & Neyret 2018, "High-Performance By-Example Noise", hex-tile
// variant — the standard published form): cover the plane with a triangular
// lattice; every lattice vertex owns a copy of the texture at its own random
// TRANSLATION; a fragment blends the three vertices of the triangle it lands in
// by the barycentric weights. Every tap is the same texture, so the result stays
// seamless and tileable, but no two neighbourhoods share an offset and the
// 5.56 m grid disappears.
//
// Two details that matter:
//  * Derivatives come from the UNOFFSET uv and are passed to textureGrad. Using
//    plain texture() would let the offset's discontinuity at a lattice edge blow
//    up the implicit derivative and pick mip 0 vs mip N on adjacent pixels — a
//    bright seam along every triangle edge. textureGrad keeps mip AND anisotropy
//    selection continuous across the lattice.
//  * The barycentric weights are sharpened by kStochContrast before blending.
//    A plain linear blend of three decorrelated copies averages toward the mean
//    and visibly washes contrast out in the transition bands; sharpening makes
//    one tap dominate over most of each cell and keeps the blends narrow.
vec2 stochOffset(vec2 v) {          // per-lattice-vertex random translation
    return fract(vec2(thash(v), thash(v + 17.31)));
}

vec3 stochSample(uint idx, vec2 uv, vec2 ddx, vec2 ddy) {
    // Skew UV into an equilateral triangular lattice and find the cell + weights.
    vec2 q  = uv * kStochLattice;
    q = vec2(q.x * 1.1547005, q.y + 0.5 * q.x * 1.1547005);
    vec2 qi = floor(q), qf = fract(q);
    vec2 v1, v2, v3; vec3 w;
    if (qf.x + qf.y < 1.0) {
        v1 = qi;                 v2 = qi + vec2(1.0, 0.0); v3 = qi + vec2(0.0, 1.0);
        w  = vec3(1.0 - qf.x - qf.y, qf.x, qf.y);
    } else {
        v1 = qi + vec2(1.0, 1.0); v2 = qi + vec2(1.0, 0.0); v3 = qi + vec2(0.0, 1.0);
        w  = vec3(qf.x + qf.y - 1.0, 1.0 - qf.y, 1.0 - qf.x);
    }
    w = pow(max(w, vec3(0.0)), vec3(kStochContrast));
    w /= max(w.x + w.y + w.z, 1e-5);

    return textureGrad(textures[nonuniformEXT(idx)], uv + stochOffset(v1), ddx, ddy).rgb * w.x
         + textureGrad(textures[nonuniformEXT(idx)], uv + stochOffset(v2), ddx, ddy).rgb * w.y
         + textureGrad(textures[nonuniformEXT(idx)], uv + stochOffset(v3), ddx, ddy).rgb * w.z;
}

// Sample a bindless detail texture by world XZ (top-down planar projection) at
// the detail tiling scale. World-space UVs => tiles seam seamlessly across the
// streamed terrain (no per-tile UV reset).
vec3 detailXZ(uint idx, vec2 worldXZ) {
    vec2 uv = worldXZ * kDetailScale;
    return stochSample(idx, uv, dFdx(uv), dFdy(uv));
}

// Triplanar sample for steep rock so vertical cliffs don't get the smeared
// top-down stretch. Blends the three world-axis planar projections by |normal|.
// Planes with negligible weight are skipped: on ordinary ground one or two of
// the three contribute nothing, which pays for the stochastic taps.
vec3 triplanar(uint idx, vec3 wpos, vec3 wn) {
    vec3 an = abs(wn);
    an = an / max(an.x + an.y + an.z, 1e-4);
    // Derivatives MUST be taken in uniform control flow (dFdx inside a branch is
    // undefined), so all three projections and their gradients are computed up
    // front; only the sampling is skipped.
    vec2 uvX = wpos.zy * kDetailScale, dXx = dFdx(uvX), dXy = dFdy(uvX);
    vec2 uvY = wpos.xz * kDetailScale, dYx = dFdx(uvY), dYy = dFdy(uvY);
    vec2 uvZ = wpos.xy * kDetailScale, dZx = dFdx(uvZ), dZy = dFdy(uvZ);
    const float kMinPlane = 0.05;
    vec3 acc = vec3(0.0); float wsum = 0.0;
    if (an.x > kMinPlane) { acc += stochSample(idx, uvX, dXx, dXy) * an.x; wsum += an.x; }
    if (an.y > kMinPlane) { acc += stochSample(idx, uvY, dYx, dYy) * an.y; wsum += an.y; }
    if (an.z > kMinPlane) { acc += stochSample(idx, uvZ, dZx, dZy) * an.z; wsum += an.z; }
    return acc / max(wsum, 1e-4);
}

// Procedural height+slope splat. Returns the blended terrain albedo in linear-ish
// sRGB (the detail textures are stored sRGB so the array already linearises them).
vec3 terrainAlbedo(vec3 wpos, vec3 wn, uvec2 pack) {
    uint grassIdx = pack.x >> 16;
    uint rockIdx  = pack.x & 0xFFFFu;
    uint snowIdx  = pack.y >> 16;
    uint sandIdx  = pack.y & 0xFFFFu;

    float h     = wpos.y;
    float slope = clamp(wn.y, 0.0, 1.0);     // 1 = flat ground, 0 = vertical

    // A bit of world noise to wobble the band boundaries so they don't read as
    // perfectly horizontal contour lines.
    float n   = tnoise(wpos.xz * kMacroScale) - 0.5;   // [-0.5,0.5]
    float hN  = h + n * 6.0;                            // height jittered by noise

    // ---- Range-theme masks (biome tints keyed off world position; centers
    // match terrain.cpp's kRanges band midpoints) ----
    float volc  = 1.0 - smoothstep(1400.0, 3200.0, distance(wpos.xz, kVolcanoXZ));
    float mesa  = 1.0 - smoothstep(2400.0, 4200.0, distance(wpos.xz, kMesaXZ));
    float moss  = 1.0 - smoothstep(1600.0, 3400.0, distance(wpos.xz, kCrystalXZ));

    // ---- Base sample the four materials (world-space UVs) ----
    vec3 grass = detailXZ(grassIdx, wpos.xz);
    vec3 sand  = detailXZ(sandIdx,  wpos.xz);
    vec3 snow  = detailXZ(snowIdx,  wpos.xz);
    // Rock uses triplanar so cliffs aren't stretched.
    vec3 rock  = triplanar(rockIdx, wpos, wn);

    // Theme the materials: dark basalt in the volcanic east, warm sandstone in
    // the southern mesas, mossier green in the western highlands.
    rock  = mix(rock, rock * vec3(0.42, 0.34, 0.32), volc);
    rock  = mix(rock, rock * vec3(1.15, 0.92, 0.68), mesa);
    grass = mix(grass, grass * vec3(0.80, 1.08, 0.72), moss);

    // ---- Height bands (smoothstep transitions, never hard) ----
    // Start as grass everywhere, then layer beach sand at the basin shoreline,
    // alpine rock with altitude, snow on the peaks, rock on slope.
    vec3 albedo = grass;

    // Beach band: strongest right at/below the basin waterline, fading out by
    // kSandTop — and gated to the SHORE (inland lows / city pads are not beach).
    float shore = 1.0 - smoothstep(950.0, 1500.0, distance(wpos.xz, kShoreXZ));
    float sandBand = (1.0 - smoothstep(kSeaLevel - 2.0, kSandTop, hN)) * shore;
    albedo = mix(albedo, sand, clamp(sandBand, 0.0, 1.0));

    // Alpine band: grass yields to rock with altitude (below the snow line).
    float alpine = smoothstep(kAlpineLo, kAlpineHi, hN);
    albedo = mix(albedo, rock, alpine * 0.9);

    // Snow cap on the high ranges: prefers flatter ground (slides off cliffs);
    // suppressed over the volcanic east (basalt stays dark).
    float snowBand = smoothstep(kSnowBottom, kSnowFull, hN)
                   * smoothstep(0.55, 0.80, slope)
                   * (1.0 - volc);
    albedo = mix(albedo, snow, clamp(snowBand, 0.0, 1.0));

    // ---- Slope rock: overrides whatever band where the surface is steep. The
    // thresholds sit high so even the gentle base field's steeper hillsides read
    // as rock; real mountainsides drop normal.y far below 0.90 and saturate. ----
    // Wobble the rock edge with noise so it isn't a clean contour line. The
    // noise jitters the SLOPE fed to the band, not the band's output: adding
    // n * 0.18 to the result (as this did) put up to 9% rock on ground that is
    // dead flat, which greyed every field and seeded the 04_saddle blobs. Jitter
    // the input and flat ground stays rock-free by construction.
    float rockBand = 1.0 - smoothstep(kSlopeRockLo, kSlopeRockHi, slope + n * 0.05);
    albedo = mix(albedo, rock, rockBand * (1.0 - snowBand * 0.55));

    // Subtle macro tint variation so large flat areas aren't a flat colour.
    float macro = tnoise(wpos.xz * (kMacroScale * 0.5));
    albedo *= mix(0.88, 1.10, macro);

    return albedo;
}
#endif  // X3_MESH_TERRAIN_GLSL
