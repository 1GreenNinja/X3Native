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
//   * SNOW lives on the high ranges only (kSnowBottom, tuned below — the base
//     field can never reach it) and prefers flatter ground (it slides off cliffs).
//   * ALPINE band: grass gives way to rock with altitude well below the snowline.
//   * Slope-rock thresholds unchanged — on real mountainsides normal.y drops far
//     below 0.90, so slopes saturate to full rock naturally.
//   * Range-theme tints (kVolcanoXZ / kMesaXZ / kCrystalXZ, matched to terrain.cpp
//     kRanges band midpoints): E volcanic = dark basalt rock + no snow; S mesa =
//     warm sandstone rock; W highlands = mossier grass.
const float kSeaLevel    = 4.0;    // world Y of the basin water surface (ocean_base)
const float kSandTop     = 16.0;   // beach fades out by here (near the basin only)
// SNOW: PAIRED TO THE TUNNEL RIDGE SUMMIT (app/terrain.cpp kRanges tunnel
// ridge — a change to that range's amp IS a change to this pair). History:
// 180/265 -> 118/185 when the ridge peaked at ~162 m (the 180 floor sat above
// the only mountain the player drives through); now the owner's raise (amp
// 285 -> 460, natural summit 289 m, 2026-08-16) put that WHOLE massif above
// the 118 line and it rendered as one pale snow dome — the exact opposite of
// the "bluish dark cliffs" the raise was for. 225/300: the dark-stone band
// (full by kAlpineTintHi 150) owns 150..225 outright, the cap starts where
// the ridge's own shoulders end, and the 289 m summit reads ~0.94 snow — a
// white CAP over dark rock, not a white mountain. The distant 300-500 m
// ranges still cap; the 55 m base field still never can.
const float kSnowBottom  = 225.0;  // snow begins (above the dark-cliff band)
const float kSnowFull    = 300.0;  // fully snow by here (high peaks)
const float kAlpineLo    = 75.0;   // grass starts yielding to rock
const float kAlpineHi    = 140.0;  // fully rock by here (below the snow line)
// HIGH-ROCK band (the SECOND rock set + tint): where the mountain stops being
// the warm roadside-cutting stone and becomes dark craggy slate. Full effect
// by 150 so the ridge summit (~162) is entirely mountain stone.
const float kAlpineTintLo = 70.0;
const float kAlpineTintHi = 150.0;
const vec3  kAlpineRock   = vec3(0.52, 0.58, 0.72);   // darker, cooler (tint fallback)
const vec3  kAlpineVein   = vec3(0.62, 0.78, 1.15);   // blue in the crevices
// CLIFF FACES GO DARK BY SLOPE, NOT ONLY BY ALTITUDE (owner 2026-08-16:
// "bluish dark cliffs" on the Large Mountain). The altitude band above still
// owns the summit; this lets a genuinely STEEP face pick up the same dark
// blue-grey stone from kCliffAltLo up, so the great lower faces of the massif
// read as cliff bands instead of roadside-cutting tan. Gated well above the
// 0..55 m rolling field so road cuttings and river banks keep the warm rock:
// both gates must pass — steepness alone never recolours lowland ground.
const float kCliffSlopeLo = 0.45;   // n.y at/below this (>63 deg) -> fully dark stone
const float kCliffSlopeHi = 0.72;   // n.y at/above this (<44 deg) -> no slope contribution
const float kCliffAltLo   = 38.0;   // dark-cliff eligibility begins (above the base field's soil)
const float kCliffAltHi   = 70.0;   // fully eligible by here (= kAlpineTintLo)
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
// --- NORMAL RELIEF (terrainNormal) ------------------------------------------
// kTerrainNormalG is the tangent-map GREEN-CHANNEL SIGN. +1 = OpenGL/glTF, -1 =
// DirectX/Unity. It is -1 here, and that is NOT a guess:
//
//  * CODE: inc/mesh_normalmap.glsl builds its derivative TBN as
//    `B = -normalize(cross(N, T))`. That explicit minus IS a green flip - it is
//    what you write when the maps you feed the frame are DirectX-convention. The
//    curated surface_library sets are the SAME assets terrain splats, so terrain
//    must apply the same flip or the ground would disagree with every prop
//    standing on it.
//  * EYE: captured both ways at the identical viewpoint
//    (docs/screenshots/terrain_normals/gflip vs tunnel_after). At +1 the tunnel
//    cut face reads soft and smeared; at -1 the cracks read RECESSED and the
//    ribs raised, which is what the albedo shows in the same pixels.
//
// If a future library set is authored the other way, fix the ASSET - do not flip
// this per-layer, or the layers would disagree with each other in the blend.
const float kTerrainNormalG = -1.0;
// Tangent XY gain. 1.0 = the map's authored relief. The curated terrain sets are
// full-strength 2K bakes; this exists so the relief can be dialled back without
// re-authoring an asset.
const float kTerrainNormalStrength = 1.0;
// A layer whose splat weight is below this contributes less than a quantisation
// step to the blended normal, so its taps are skipped. Ordinary ground is grass
// only (3 taps); a cliff pays grass + rock. WITHOUT this gate every terrain
// fragment would pay all four layers' normals on top of all four albedos —
// up to 36 texture fetches — for a contribution of exactly zero.
const float kLayerEps = 0.004;
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

// ===========================================================================
// FOOTPRINT ANTI-SHIMMER (outdoor-polish lane; ssao.terrainMat.x).
//
// THE DEFECT, stated precisely. The mip chain filters what is INSIDE each tap.
// It does nothing at all for the WEIGHTS BETWEEN taps, and the hex-lattice
// blend above computes those weights analytically, per pixel, from a lattice
// whose period is 1/(kDetailScale*kStochLattice) ~= 15.9 WORLD METRES.
//
// At the range the owner is looking (the massif sits 7-10 km out) one 720p
// pixel spans several metres of ground -- comparable to, or wider than, that
// lattice cell. Adjacent pixels therefore land in DIFFERENT lattice triangles
// with different random offsets, and a sub-texel camera move re-rolls which
// triangle each pixel lands in. That is a per-frame re-roll of a
// full-contrast 3-way blend: it SPARKLES, and no amount of anisotropy or mip
// bias can touch it, because the aliasing is in the blend, not the fetch.
//
// The fix is the one the technique's own premise implies: the stochastic
// offset exists ONLY to hide a visible 5.56 m repeat. Once a pixel is wider
// than the repeat there is no repeat left to hide, so fade back to the plain,
// un-offset, lattice-independent tap. Continuous (a lerp), free where it
// matters (the near-field branch below takes exactly the old three taps), and
// CHEAPER at distance (one tap instead of three).
//
// Returns 1 = full stochastic blend (near), 0 = plain single tap (far).
// `ddx/ddy` are the UV-cycle derivatives already computed by every caller.
float stochBlend(vec2 ddx, vec2 ddy) {
    // Pixel footprint measured in LATTICE CELLS. kStochLattice converts
    // uv-cycles to lattice units, which is the unit the aliasing lives in.
    float latFp = max(length(ddx), length(ddy)) * kStochLattice;
    // Below ~1/8 cell per pixel the lattice is comfortably resolved; past ~0.6
    // it is being point-sampled and every weight is noise.
    float fade = smoothstep(0.12, 0.60, latFp);
    return mix(1.0, 1.0 - fade, clamp(ssao.terrainMat.x, 0.0, 1.0));
}

// Same footprint metric expressed in WORLD METRES per pixel, for the callers
// that need to fade something whose scale is authored in world units (the
// splat-mask noise, the normal relief). `wpos` must come from a varying, so
// this may only be called in uniform control flow.
float terrainFootprintM(vec3 wpos) {
    return max(length(dFdx(wpos)), length(dFdy(wpos)));
}

// Resolve the lattice ONCE for a uv: the three neighbouring vertices' random
// translations + their sharpened barycentric weights. Split out of stochSample so
// the NORMAL map can be fetched at literally the same three offsets with the same
// three weights as its albedo. If the normal took its own lattice, the two would
// decorrelate and every triangle edge would show a shading crease the colour
// doesn't have — the exact seam class this technique exists to remove.
void stochLattice(vec2 uv, out vec2 o1, out vec2 o2, out vec2 o3, out vec3 w) {
    // Skew UV into an equilateral triangular lattice and find the cell + weights.
    vec2 q  = uv * kStochLattice;
    q = vec2(q.x * 1.1547005, q.y + 0.5 * q.x * 1.1547005);
    vec2 qi = floor(q), qf = fract(q);
    vec2 v1, v2, v3;
    if (qf.x + qf.y < 1.0) {
        v1 = qi;                 v2 = qi + vec2(1.0, 0.0); v3 = qi + vec2(0.0, 1.0);
        w  = vec3(1.0 - qf.x - qf.y, qf.x, qf.y);
    } else {
        v1 = qi + vec2(1.0, 1.0); v2 = qi + vec2(1.0, 0.0); v3 = qi + vec2(0.0, 1.0);
        w  = vec3(qf.x + qf.y - 1.0, 1.0 - qf.y, 1.0 - qf.x);
    }
    w = pow(max(w, vec3(0.0)), vec3(kStochContrast));
    w /= max(w.x + w.y + w.z, 1e-5);
    o1 = stochOffset(v1); o2 = stochOffset(v2); o3 = stochOffset(v3);
}

// textureGrad carries EXPLICIT gradients, so unlike texture()/dFdx it is legal
// in non-uniform control flow — which is what lets the three branches below
// exist at all. b == 1 takes literally the old three taps (byte-identical, and
// the only path r_terrainaa 0 can reach); b == 0 takes ONE.
vec3 stochSample(uint idx, vec2 uv, vec2 ddx, vec2 ddy) {
    float b = stochBlend(ddx, ddy);
    if (b >= 0.998) {
        vec2 o1, o2, o3; vec3 w;
        stochLattice(uv, o1, o2, o3, w);
        return textureGrad(textures[nonuniformEXT(idx)], uv + o1, ddx, ddy).rgb * w.x
             + textureGrad(textures[nonuniformEXT(idx)], uv + o2, ddx, ddy).rgb * w.y
             + textureGrad(textures[nonuniformEXT(idx)], uv + o3, ddx, ddy).rgb * w.z;
    }
    vec3 plain = textureGrad(textures[nonuniformEXT(idx)], uv, ddx, ddy).rgb;
    if (b <= 0.002) return plain;
    vec2 o1, o2, o3; vec3 w;
    stochLattice(uv, o1, o2, o3, w);
    vec3 stoch = textureGrad(textures[nonuniformEXT(idx)], uv + o1, ddx, ddy).rgb * w.x
               + textureGrad(textures[nonuniformEXT(idx)], uv + o2, ddx, ddy).rgb * w.y
               + textureGrad(textures[nonuniformEXT(idx)], uv + o3, ddx, ddy).rgb * w.z;
    return mix(plain, stoch, b);
}

// The same three taps, read as a TANGENT-SPACE normal. Decoded to [-1,1] per tap
// and weight-summed; because the weights are normalised this is identical to
// decoding the blended bytes, and it keeps the intent readable. textureGrad with
// the UNOFFSET derivatives is load-bearing here for the same reason it is for
// albedo: the offset is discontinuous at a lattice edge, and an implicit
// derivative would pick a different mip on adjacent pixels.
vec3 stochNormalTS(uint idx, vec2 uv, vec2 ddx, vec2 ddy) {
    float b = stochBlend(ddx, ddy);
    vec3 t;
    if (b >= 0.998) {
        vec2 o1, o2, o3; vec3 w;
        stochLattice(uv, o1, o2, o3, w);
        t = textureGrad(textures[nonuniformEXT(idx)], uv + o1, ddx, ddy).xyz * w.x
          + textureGrad(textures[nonuniformEXT(idx)], uv + o2, ddx, ddy).xyz * w.y
          + textureGrad(textures[nonuniformEXT(idx)], uv + o3, ddx, ddy).xyz * w.z;
    } else {
        vec3 plain = textureGrad(textures[nonuniformEXT(idx)], uv, ddx, ddy).xyz;
        if (b <= 0.002) {
            t = plain;
        } else {
            vec2 o1, o2, o3; vec3 w;
            stochLattice(uv, o1, o2, o3, w);
            vec3 s = textureGrad(textures[nonuniformEXT(idx)], uv + o1, ddx, ddy).xyz * w.x
                   + textureGrad(textures[nonuniformEXT(idx)], uv + o2, ddx, ddy).xyz * w.y
                   + textureGrad(textures[nonuniformEXT(idx)], uv + o3, ddx, ddy).xyz * w.z;
            t = mix(plain, s, b);
        }
    }
    t = t * 2.0 - 1.0;
    t.y *= kTerrainNormalG;
    t.xy *= kTerrainNormalStrength;
    return t;
}

// --- TRIPLANAR / PLANAR NORMAL REORIENTATION --------------------------------
// Terrain has no vertex tangents and no UVs — its albedo is projected from world
// position, so its normal must be too. The WHITEOUT BLEND (the standard published
// triplanar-normal form: add the geometry normal's two off-axis components into
// the tangent normal's xy, keep |z| scaled by the plane weight, then swizzle the
// result back into world space) reorients each planar tangent normal onto the
// actual geometry normal without ever building a TBN. It is cheap, stable at
// grazing angles, and — unlike naive "swizzle and add" — it cannot flip a normal
// through the surface on a steep face.
//
// Y PLANE (uv = wpos.xz), used by grass / sand / snow, which are top-down only.
vec3 whiteoutY(vec3 tn, vec3 wn) {
    return vec3(tn.xy + wn.xz, abs(tn.z) * wn.y).xzy;
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

// Triplanar NORMAL, tap-for-tap the mirror of triplanar() above: the same three
// projections, the same |normal| plane weights, the same kMinPlane skip, the same
// stochastic lattice. Returns an UNNORMALIZED world-space normal (the caller
// blends several layers before normalizing once).
vec3 triplanarNormal(uint idx, vec3 wpos, vec3 wn) {
    vec3 an = abs(wn);
    an = an / max(an.x + an.y + an.z, 1e-4);
    vec2 uvX = wpos.zy * kDetailScale, dXx = dFdx(uvX), dXy = dFdy(uvX);
    vec2 uvY = wpos.xz * kDetailScale, dYx = dFdx(uvY), dYy = dFdy(uvY);
    vec2 uvZ = wpos.xy * kDetailScale, dZx = dFdx(uvZ), dZy = dFdy(uvZ);
    const float kMinPlane = 0.05;
    vec3 acc = vec3(0.0); float wsum = 0.0;
    if (an.x > kMinPlane) {
        vec3 t = stochNormalTS(idx, uvX, dXx, dXy);
        acc += vec3(t.xy + wn.zy, abs(t.z) * wn.x).zyx * an.x; wsum += an.x;
    }
    if (an.y > kMinPlane) {
        vec3 t = stochNormalTS(idx, uvY, dYx, dYy);
        acc += whiteoutY(t, wn) * an.y; wsum += an.y;
    }
    if (an.z > kMinPlane) {
        vec3 t = stochNormalTS(idx, uvZ, dZx, dZy);
        acc += vec3(t.xy + wn.xy, abs(t.z) * wn.z) * an.z; wsum += an.z;
    }
    return acc / max(wsum, 1e-4);
}
// Extracted verbatim from terrainAlbedo (same expressions, same order, same
// noise) so that terrainNormal can blend each layer's normal map with EXACTLY
// the weight its albedo got. Anything less and the colour of a rock face would
// arrive with the relief of the grass under it.
struct TerrainSplat {
    float sand;    // beach band, already clamped
    float alpine;  // altitude rock, pre-0.9 scale
    float snow;    // snow cap, already clamped
    float rock;    // slope rock, pre snow-suppression
    float volc, mesa, moss;   // range-theme masks (albedo tint only)
    float macro;              // macro tint (albedo only)
    float hN;                 // noise-jittered height, shared with the alpine band
};

// How much of a world-space detail term of wavelength `wlM` survives at a pixel
// footprint of `fpM` metres. A term whose wavelength is under a couple of pixels
// contributes nothing but per-frame noise: it cannot be SEEN as structure, only
// as sparkle, so it is faded out rather than point-sampled. Gated by
// ssao.terrainMat.x so r_terrainaa 0 returns 1.0 and the historical math runs.
float terrainDetailFade(float fpM, float wlM) {
    float f = 1.0 - smoothstep(wlM * 0.10, wlM * 0.45, fpM);
    return mix(1.0, f, clamp(ssao.terrainMat.x, 0.0, 1.0));
}

// How much of the tangent-space normal relief survives at this footprint.
// 1 = the authored relief, 0 = the geometry normal alone. Shared by
// terrainNormal() (which blends the normal) and terrainSurface() (which converts
// the lost normal variance into roughness), so the two can never disagree.
// The 6.0 m "wavelength" puts terrainDetailFade's band at 0.60 .. 2.70 m/pixel.
float terrainReliefAmount(float fpM) { return terrainDetailFade(fpM, 6.0); }

// ===========================================================================
// AUTHORED PER-BAND SURFACE (ssao.terrainMat.y/.z/.w).
//
// Terrain used ONE flat dielectric roughness -- 0.5 -- for grass, cliff rock,
// snow and sand alike, because mesh.frag's untextured path has a single
// constant and terrain has no MR map. Grass and a wind-packed snow crust do not
// return light the same way, and the difference is most of what "reads real" at
// a low sun. These are the dials; retune HERE, one place, not per call site.
//
//   rough : GGX perceptual roughness. Lower = tighter, brighter highlight.
//   spec  : scale on the dielectric F0 (0.04). >1 = a wetter/harder mineral
//           surface, <1 = a dusty one that barely returns a specular lobe.
//
// SAND is the owner's calibration band ("hard baked sand shimmering in the
// evening glow"): ssao.terrainMat.z crossfades it between MATTE and a glossy
// hardpan that throws a real grazing glint. That is a deliberate MATERIAL
// sparkle, which is exactly why the footprint aliasing had to be fixed first --
// otherwise there is no way to tell the intended glint from the broken one.
const float kRoughGrass = 0.88, kSpecGrass = 0.55;   // soft, near-lambertian
const float kRoughRock  = 0.74, kSpecRock  = 1.00;   // stone: a real, broad lobe
const float kRoughSnow  = 0.52, kSpecSnow  = 1.25;   // crusted facets glint
const float kRoughSandM = 0.82, kSpecSandM = 0.45;   // MATTE loose sand
const float kRoughSandS = 0.30, kSpecSandS = 1.55;   // SPARKLE: baked hardpan

struct TerrainSurface { float rough; float spec; };

TerrainSplat terrainSplatWeights(vec3 wpos, vec3 wn, float fpM) {
    TerrainSplat s;
    float h     = wpos.y;
    float slope = clamp(wn.y, 0.0, 1.0);     // 1 = flat ground, 0 = vertical

    // A bit of world noise to wobble the band boundaries so they don't read as
    // perfectly horizontal contour lines. FADED WITH FOOTPRINT: at 8 km a pixel
    // spans several metres, so a metre-scale wobble on a band EDGE is a
    // per-frame coin flip between two materials -- the band edge crawls.
    float n   = (tnoise(wpos.xz * kMacroScale) - 0.5)
              * terrainDetailFade(fpM, 1.0 / kMacroScale);   // ~83 m wavelength
    float hN  = h + n * 6.0;                            // height jittered by noise
    s.hN = hN;                                          // published for terrainAlbedo

    // ---- Range-theme masks (biome tints keyed off world position; centers
    // match terrain.cpp's kRanges band midpoints) ----
    s.volc = 1.0 - smoothstep(1400.0, 3200.0, distance(wpos.xz, kVolcanoXZ));
    s.mesa = 1.0 - smoothstep(2400.0, 4200.0, distance(wpos.xz, kMesaXZ));
    s.moss = 1.0 - smoothstep(1600.0, 3400.0, distance(wpos.xz, kCrystalXZ));

    // Beach band: strongest right at/below the basin waterline, fading out by
    // kSandTop — and gated to the SHORE (inland lows / city pads are not beach).
    float shore = 1.0 - smoothstep(950.0, 1500.0, distance(wpos.xz, kShoreXZ));
    s.sand = clamp((1.0 - smoothstep(kSeaLevel - 2.0, kSandTop, hN)) * shore, 0.0, 1.0);

    // Alpine band: grass yields to rock with altitude (below the snow line).
    s.alpine = smoothstep(kAlpineLo, kAlpineHi, hN);

    // Snow cap on the high ranges: prefers flatter ground (slides off cliffs);
    // suppressed over the volcanic east (basalt stays dark).
    // WEATHER LOWERS THE SNOWLINE. The altitude band below is the permanent
    // one -- the cap these peaks wear in fair weather. A snowstorm does not
    // paint the world white uniformly; it brings the LINE DOWN, so the tops go
    // first and the valleys go last, because height is cold. Watching the white
    // come down the mountain as it settles is the whole effect, and it is what
    // uniform whitening throws away.
    //
    // At full cover the band bottoms out below sea level, so even flat ground
    // is under it -- but it arrives there LAST, having swept down the range.
    float snowW = clamp(ssao.precip.x, 0.0, 1.0);
    // At full cover the band sits well BELOW the valleys, so even the ragged
    // edge below cannot punch bare holes in a whiteout. A blizzard buries
    // everything; the raggedness is a property of a MARGINAL snowline.
    float snowBot = mix(kSnowBottom, -45.0, snowW);
    float snowTop = mix(kSnowFull,   -12.0, snowW);

    // ---- A RAGGED SNOWLINE -------------------------------------------------
    // A snowline is never a contour. It wanders tens of metres vertically with
    // aspect and wind, and it sends FINGERS down gullies while leaving ribs
    // bare. The clean smoothstep this replaces drew a perfect horizontal band
    // around the massif, and a perfect band is the single loudest tell that a
    // mountain was generated rather than observed -- no amount of texture work
    // fixes a line that straight.
    //
    // Two octaves, because one is a wobble and two is terrain: the coarse term
    // (~240 m) makes the whole line rise and fall across the range, the fine
    // one (~28 m) breaks its edge into fingers and islands.
    //
    // JITTER THE INPUT, NOT THE OUTPUT -- the same rule the slope-rock band
    // below learned the hard way. Adding noise to the RESULT would sprinkle
    // snow onto ground nowhere near the line; moving the height the band is
    // measured at keeps deep valleys bare and high peaks white by construction.
    // Both octaves fade with the pixel footprint (see terrainDetailFade): the
    // FINE one (~28 m) is the first thing to go sub-pixel on a distant massif,
    // and it is the loudest shimmer term on the snowline because it modulates a
    // full-contrast white/rock edge.
    float snowCoarse = (tnoise(wpos.xz * (kMacroScale * 0.35)) - 0.5)
                     * terrainDetailFade(fpM, 1.0 / (kMacroScale * 0.35));   // ~238 m
    float snowFine   = (tnoise(wpos.xz * (kMacroScale * 3.00)) - 0.5)
                     * terrainDetailFade(fpM, 1.0 / (kMacroScale * 3.00));   // ~28 m
    // Fade the raggedness as cover rises: the line is only ragged while it is
    // marginal. Under a full fall there is no edge left to be ragged.
    float ragged = 1.0 - 0.6 * snowW;
    float hSnow  = hN + (snowCoarse * 26.0 + snowFine * 7.0) * ragged;

    // Deep snow also holds on ground it would otherwise slide off. Relaxing the
    // slope gate is what lets it climb the cutting walls and the road banks
    // instead of leaving them bare in a whiteout.
    float slopeLo = 0.55 - 0.28 * snowW;
    float slopeHi = 0.80 - 0.22 * snowW;
    // WIND SCOUR. Snow does not lie evenly on exposed ground -- wind strips it
    // off ribs and packs it into hollows, which is why a real snowfield is
    // mottled rather than a bedsheet. Fed into the SLOPE for the same reason as
    // above: dead-flat ground stays covered no matter what the noise says,
    // because flat ground is where snow actually collects.
    float scour = snowFine * 0.10;
    s.snow = clamp(smoothstep(snowBot, snowTop, hSnow)
                 * smoothstep(slopeLo, slopeHi, slope + scour)
                 * (1.0 - s.volc), 0.0, 1.0);

    // ---- Slope rock: overrides whatever band where the surface is steep. ----
    // Wobble the rock edge with noise so it isn't a clean contour line. The
    // noise jitters the SLOPE fed to the band, not the band's output: adding
    // n * 0.18 to the result (as this once did) put up to 9% rock on ground that
    // is dead flat, which greyed every field and seeded the 04_saddle blobs.
    // Jitter the input and flat ground stays rock-free by construction.
    s.rock = 1.0 - smoothstep(kSlopeRockLo, kSlopeRockHi, slope + n * 0.05);

    // Subtle macro tint variation so large flat areas aren't a flat colour.
    s.macro = tnoise(wpos.xz * (kMacroScale * 0.5));
    return s;
}

// Blend the band table with EXACTLY the weights terrainAlbedo uses, in the same
// order, so a rock face's colour never arrives with the grass's gloss.
TerrainSurface terrainSurface(vec3 wpos, vec3 wn) {
    float fpM = terrainFootprintM(wpos);
    TerrainSplat s = terrainSplatWeights(wpos, wn, fpM);

    float sparkle = clamp(ssao.terrainMat.z, 0.0, 1.0);
    float rSand = mix(kRoughSandM, kRoughSandS, sparkle);
    float sSand = mix(kSpecSandM,  kSpecSandS,  sparkle);

    float r = kRoughGrass, sp = kSpecGrass;
    r  = mix(r,  rSand,      s.sand);
    sp = mix(sp, sSand,      s.sand);
    r  = mix(r,  kRoughRock, s.alpine * 0.9);
    sp = mix(sp, kSpecRock,  s.alpine * 0.9);
    r  = mix(r,  kRoughSnow, s.snow);
    sp = mix(sp, kSpecSnow,  s.snow);
    float rockW = s.rock * (1.0 - s.snow * 0.55);
    r  = mix(r,  kRoughRock, rockW);
    sp = mix(sp, kSpecRock,  rockW);

    // SPECULAR ANTI-ALIASING. Where the relief has been faded out (see
    // terrainReliefAmount) the surface has lost normal VARIANCE it used to
    // scatter with. Putting that variance back as roughness is the standard
    // normal-variance -> roughness conversion, and it is what stops a distant
    // slope from turning into a field of pinpoint highlights the moment its
    // bumps stop resolving. Deliberately UNCONDITIONAL on sparkle: the sand
    // glint is a NEAR-field look and must not survive to 8 km either.
    float lost = 1.0 - terrainReliefAmount(fpM);
    r = mix(r, max(r, 0.85), lost);

    TerrainSurface o;
    o.rough = clamp(r * max(ssao.terrainMat.w, 0.05), 0.045, 1.0);
    o.spec  = sp;
    return o;
}

// Procedural height+slope splat. Returns the blended terrain albedo in linear-ish
// sRGB (the detail textures are stored sRGB so the array already linearises them).
vec3 terrainAlbedo(vec3 wpos, vec3 wn, uvec2 pack) {
    // Lanes are 16-bit but bindless indices are < 4096 (12 bits), so each lane's
    // top nibble is spare — the OPTIONAL 5th index (high-altitude rock) rides
    // three of them (packed in vk_passes.cpp; must mirror exactly):
    //   pack.x bits 28-31 = rockHigh[11:8]
    //   pack.x bits 12-15 = rockHigh[7:4]
    //   pack.y bits 28-31 = rockHigh[3:0]
    // MASK EVERY LANE WITH 0xFFF. Reading the full 16 bits (as this did before
    // the merge) folds those piggyback nibbles into the ids and corrupts all
    // four the moment a high-rock set is registered.
    uint grassIdx  = (pack.x >> 16) & 0xFFFu;
    uint rockIdx   =  pack.x        & 0xFFFu;
    uint snowIdx   = (pack.y >> 16) & 0xFFFu;
    uint sandIdx   =  pack.y        & 0xFFFu;
    uint rockHiIdx = (((pack.x >> 28) & 0xFu) << 8)
                   | (((pack.x >> 12) & 0xFu) << 4)
                   |  ((pack.y >> 28) & 0xFu);      // 0 = no high set registered

    TerrainSplat s = terrainSplatWeights(wpos, wn, terrainFootprintM(wpos));

    // ---- Base sample the four materials (world-space UVs) ----
    vec3 grass = detailXZ(grassIdx, wpos.xz);
    vec3 sand  = detailXZ(sandIdx,  wpos.xz);
    // SNOW IS TRIPLANAR, like the rock it lies on. It was top-down only, which
    // is fine on a field and wrong on a mountain: a near-vertical face has
    // almost no XZ footprint, so the top-down projection SMEARS the tile into
    // vertical streaks down the whole massif. Those streaks were the last thing
    // reading as fake once the texture itself was fixed -- and the fix is the
    // one rock already had for exactly this reason, sitting one line below.
    // Snow is the only other layer that lives on steep ground, so it is the only
    // other one that needs it; grass and sand stay cheap and top-down.
    vec3 snow  = triplanar(snowIdx, wpos, wn);
    // Rock uses triplanar so cliffs aren't stretched.
    vec3 rock  = triplanar(rockIdx, wpos, wn);

    // Theme the materials: dark basalt in the volcanic east, warm sandstone in
    // the southern mesas, mossier green in the western highlands.
    rock  = mix(rock, rock * vec3(0.42, 0.34, 0.32), s.volc);
    rock  = mix(rock, rock * vec3(1.15, 0.92, 0.68), s.mesa);
    grass = mix(grass, grass * vec3(0.80, 1.08, 0.72), s.moss);

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
        float alt = smoothstep(kAlpineTintLo, kAlpineTintHi, s.hN);
        // Steep faces join the dark-stone band from kCliffAltLo up (see the
        // kCliffSlope* block comment). max(), not +: a steep face high on the
        // massif is already fully dark and must not overshoot the crossfade.
        alt = max(alt, (1.0 - smoothstep(kCliffSlopeLo, kCliffSlopeHi, clamp(wn.y, 0.0, 1.0)))
                       * smoothstep(kCliffAltLo, kCliffAltHi, s.hN));
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
    albedo = mix(albedo, sand, s.sand);
    albedo = mix(albedo, rock, s.alpine * 0.9);
    albedo = mix(albedo, snow, s.snow);
    albedo = mix(albedo, rock, s.rock * (1.0 - s.snow * 0.55));

    // Subtle macro tint variation so large flat areas aren't a flat colour.
    albedo *= mix(0.88, 1.10, s.macro);

    return albedo;
}

// ---------------------------------------------------------------------------
// TERRAIN NORMAL RELIEF.
//
// THE BUG THIS FIXES: mesh.frag gated its whole normal-map path behind
// `(vFlags & FLAG_TERRAIN) == 0u`. Every terrain fragment in every world
// therefore shaded from the geometry normal alone — the splat chose a rock
// COLOUR for a cut face and then lit it as if it were polished plaster. No
// amount of picking a better cliff albedo could fix that, because the missing
// signal was never in the albedo.
//
// `nrmPack` mirrors the albedo pack's layout: x = grass<<16|rock,
// y = snow<<16|sand, all four being bindless indices into the same array.
// ZERO IS "NO MAP": if the host never registered normals (headless, a fetch the
// box hasn't run, an unpublished set) every index is 0 and this returns the
// geometry normal unchanged — byte-identical to the pre-relief renderer.
//
// Blending: the SAME mix chain, in the SAME order, with the SAME weights as
// terrainAlbedo, over world-space normals; normalized once at the end. Rock's
// triplanar is fetched once and reused by both the alpine and slope-rock mixes,
// exactly as the albedo does.
vec3 terrainNormal(vec3 wpos, vec3 wn, uvec2 nrmPack) {
    uint grassN = nrmPack.x >> 16;
    uint rockN  = nrmPack.x & 0xFFFFu;
    uint snowN  = nrmPack.y >> 16;
    uint sandN  = nrmPack.y & 0xFFFFu;
    if ((grassN | rockN | snowN | sandN) == 0u) return wn;

    // RELIEF FADE (ssao.terrainMat.x). The normal maps are sub-metre bakes. Past
    // ~1 m per pixel their structure cannot be RESOLVED, only sampled -- and a
    // per-frame-resampled normal feeding a specular lobe is precisely the
    // "mountain sparkles" report. Fade the relief back to the geometry normal
    // over the footprint band; the mountain keeps its SHAPE (geometry + splat)
    // and loses only detail the pixel was never wide enough to show. The SAME
    // fade raises roughness in terrainSurface() below, so the energy the flattened
    // normal stops scattering is put back as a broader lobe rather than lost.
    //
    // NO EARLY-OUT HERE, deliberately: every dFdx below must stay in uniform
    // control flow, so the fade is applied as a blend at the very end instead.
    float fpM    = terrainFootprintM(wpos);
    float relief = terrainReliefAmount(fpM);

    TerrainSplat s = terrainSplatWeights(wpos, wn, fpM);

    // Derivatives for the shared top-down projection, hoisted OUT of every
    // branch below (dFdx in non-uniform control flow is undefined) — the same
    // discipline triplanar() uses, and the reason the layer skips are safe.
    vec2 uvY = wpos.xz * kDetailScale;
    vec2 dYx = dFdx(uvY), dYy = dFdy(uvY);

    vec3 N = (grassN != 0u) ? whiteoutY(stochNormalTS(grassN, uvY, dYx, dYy), wn) : wn;

    if (s.sand > kLayerEps && sandN != 0u)
        N = mix(N, whiteoutY(stochNormalTS(sandN, uvY, dYx, dYy), wn), s.sand);

    // SNOW'S NORMAL IS TRIPLANAR TOO, matching its albedo one function up. If
    // only the colour were reprojected the two would disagree on every steep
    // face -- unstretched snow lit by stretched slopes -- which is a worse
    // artefact than the stretch was, because it looks like a lighting bug
    // rather than a texture one. Sampled once here and reused by both branches.
    vec3 snowNrm = (snowN != 0u) ? triplanarNormal(snowN, wpos, wn) : wn;

    // Rock is wanted by BOTH the alpine band and the slope band; sample once.
    float rockW1 = s.alpine * 0.9;
    float rockW2 = s.rock * (1.0 - s.snow * 0.55);
    if ((rockW1 > kLayerEps || rockW2 > kLayerEps) && rockN != 0u) {
        vec3 rockNrm = triplanarNormal(rockN, wpos, wn);
        N = mix(N, rockNrm, rockW1);
        if (s.snow > kLayerEps && snowN != 0u)
            N = mix(N, snowNrm, s.snow);
        N = mix(N, rockNrm, rockW2);
    } else if (s.snow > kLayerEps && snowN != 0u) {
        N = mix(N, snowNrm, s.snow);
    }

    // A degenerate blend (opposing layers cancelling) would normalize to noise;
    // fall back to the geometry normal rather than shade from garbage.
    float len = length(N);
    N = (len > 1e-4) ? N / len : wn;
    // ...then fade the whole relief toward the geometry normal by footprint.
    // The >= 0.9995 early-out RETURNS N untouched rather than running a no-op
    // mix + renormalize through it: that keeps r_terrainaa 0 (and every
    // near-field fragment) BIT-identical, which a redundant normalize would not.
    // Safe as a branch — no derivative is taken past this point.
    if (relief >= 0.9995) return N;
    N = mix(wn, N, relief);
    float len2 = length(N);
    return (len2 > 1e-4) ? N / len2 : wn;
}
#endif  // X3_MESH_TERRAIN_GLSL
