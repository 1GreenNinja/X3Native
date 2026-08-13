#ifndef X3_MESH_TERRAIN_GLSL
#define X3_MESH_TERRAIN_GLSL
// ===========================================================================
// Terrain material splat (open-world ground). Procedural height + slope blend of
// up to five tiling detail textures (grass / rock / snow / sand + an optional
// high-altitude rock) keyed off the fragment's WORLD position + WORLD surface
// normal, with a little value noise to break the tiling. Clean-room: built from the public height/slope-splat +
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
// SNOW: retuned 180/265 -> 118/185 for the TUNNEL RIDGE (peak ~162 m — the old
// 180 m floor sat ABOVE it, so the one mountain the player drives through
// could never cap). Still comfortably above the 55 m base field (no lowland
// snow); the four distant 400-500 m ranges just cap lower down, which is what
// ranges do.
const float kSnowBottom  = 118.0;  // snow begins (mountain shoulders)
const float kSnowFull    = 185.0;  // fully snow by here (high peaks)
const float kAlpineLo    = 75.0;   // grass starts yielding to rock
const float kAlpineHi    = 130.0;
// HIGH-ROCK band (the SECOND rock set + tint): where the mountain stops being
// the warm roadside-cutting stone and becomes dark craggy slate. Full effect
// by 150 so the ridge summit (~162) is entirely mountain stone.
const float kAlpineTintLo = 70.0;
const float kAlpineTintHi = 150.0;
const vec3  kAlpineRock   = vec3(0.52, 0.58, 0.72);   // darker, cooler (tint fallback)
const vec3  kAlpineVein   = vec3(0.62, 0.78, 1.15);   // blue in the crevices  // fully rock by here (below the snow line)
const float kSlopeRockLo = 0.90;   // normal.y at/below this -> full rock (steep)
const float kSlopeRockHi = 0.965;  // normal.y at/above this -> no rock (flat)
const float kDetailScale = 0.18;   // world-space detail tiling (cycles / meter)
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

// Sample a bindless detail texture by world XZ (top-down planar projection) at
// the detail tiling scale. World-space UVs => tiles seam seamlessly across the
// streamed terrain (no per-tile UV reset).
vec3 detailXZ(uint idx, vec2 worldXZ) {
    return texture(textures[nonuniformEXT(idx)], worldXZ * kDetailScale).rgb;
}

// Triplanar sample for steep rock so vertical cliffs don't get the smeared
// top-down stretch. Blends the three world-axis planar projections by |normal|.
vec3 triplanar(uint idx, vec3 wpos, vec3 wn) {
    vec3 an = abs(wn);
    an = an / max(an.x + an.y + an.z, 1e-4);
    vec3 cx = texture(textures[nonuniformEXT(idx)], wpos.zy * kDetailScale).rgb;
    vec3 cy = texture(textures[nonuniformEXT(idx)], wpos.xz * kDetailScale).rgb;
    vec3 cz = texture(textures[nonuniformEXT(idx)], wpos.xy * kDetailScale).rgb;
    return cx * an.x + cy * an.y + cz * an.z;
}

// Procedural height+slope splat. Returns the blended terrain albedo in linear-ish
// sRGB (the detail textures are stored sRGB so the array already linearises them).
vec3 terrainAlbedo(vec3 wpos, vec3 wn, uvec2 pack) {
    // Lanes are 16-bit but bindless indices are < 4096 (12 bits), so each
    // lane's top nibble is spare — the OPTIONAL 5th index (high-altitude rock)
    // rides three of them (packed in vk_passes.cpp; must mirror exactly):
    //   pack.x bits 28-31 = rockHigh[11:8]
    //   pack.x bits 12-15 = rockHigh[7:4]
    //   pack.y bits 28-31 = rockHigh[3:0]
    // Mask every lane with 0xFFF or the piggyback nibbles corrupt the ids.
    uint grassIdx  = (pack.x >> 16) & 0xFFFu;
    uint rockIdx   =  pack.x        & 0xFFFu;
    uint snowIdx   = (pack.y >> 16) & 0xFFFu;
    uint sandIdx   =  pack.y        & 0xFFFu;
    uint rockHiIdx = (((pack.x >> 28) & 0xFu) << 8)
                   | (((pack.x >> 12) & 0xFu) << 4)
                   |  ((pack.y >> 28) & 0xFu);      // 0 = no high set registered

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

    // ---- HIGH-ALTITUDE ROCK: the SECOND rock band ---------------------------
    // The low rock slot is shared with the road cuttings (~15 m), so the massif
    // cannot simply swap the set — it CROSSFADES to a second, darker craggy set
    // with altitude when one is registered (terrain_rock_dark; rockHiIdx != 0).
    // The vein pass runs on whichever stone the altitude picked: the set's own
    // dark crevices are pushed COOL, so the mineral streaks the texture already
    // has read as blue veins in the shadowed relief — luminance re-mapping, not
    // added noise, so it never fights the relief.
    //
    // Without a registered high set (rockHiIdx == 0 — any 4-texture caller)
    // this degrades to the previous behaviour exactly: the same TINT on the one
    // shared rock set.
    {
        float alt = smoothstep(kAlpineTintLo, kAlpineTintHi, hN);
        if (alt > 0.0) {
            vec3 hi;
            if (rockHiIdx != 0u) {
                hi = triplanar(rockHiIdx, wpos, wn);          // real mountain stone
                // The dark set is already cool; the tint pass only deepens it a
                // touch so the summit doesn't flatten to one value.
                hi = mix(hi, hi * vec3(0.86, 0.90, 1.02), alt * 0.6);
            } else {
                hi = rock * kAlpineRock;                      // tint-only fallback
            }
            float lum  = dot(hi, vec3(0.299, 0.587, 0.114));
            float dark = 1.0 - smoothstep(0.18, 0.55, lum);   // 1 in the crevices
            hi = mix(hi, hi * kAlpineVein, dark);             // veins in the recesses
            rock = mix(rock, hi, alt);
        }
    }

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
    float rockBand = 1.0 - smoothstep(kSlopeRockLo, kSlopeRockHi, slope);
    // Wobble the rock edge with noise so it isn't a clean contour line.
    rockBand = clamp(rockBand + n * 0.18, 0.0, 1.0);
    albedo = mix(albedo, rock, rockBand * (1.0 - snowBand * 0.55));

    // Subtle macro tint variation so large flat areas aren't a flat colour.
    float macro = tnoise(wpos.xz * (kMacroScale * 0.5));
    albedo *= mix(0.88, 1.10, macro);

    return albedo;
}
#endif  // X3_MESH_TERRAIN_GLSL
