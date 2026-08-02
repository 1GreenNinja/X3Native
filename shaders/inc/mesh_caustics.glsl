#ifndef X3_MESH_CAUSTICS_GLSL
#define X3_MESH_CAUSTICS_GLSL
// ===========================================================================
// UNDERWATER CAUSTICS (ssao.caustics: x = enabled, y = local water surface Y,
// z = time, w = intensity). Purely procedural — no textures, no extra pass.
//
// THE TRICK (the classic two-layer min): build two INDEPENDENT interference
// webs — each a ridge field over three non-axis-aligned directional sine waves
// (ridges where the summed wave crosses zero, i.e. a travelling cellular web) —
// at different world scales, drifting in different directions and evolving at
// different rates; min() keeps light only where BOTH webs are ridged, which is
// exactly the sharp branching FILAMENT topology real caustics have, and a
// power curve then sharpens the filaments so they read as focused light, not
// as a projected texture. Sampled at worldPos.xz: caustics are cast straight
// down through a locally-flat surface (the host passes the CAMERA-LOCAL water
// level — the river is flat per-reach and the sea is flat, so no spline math
// ever runs here).
//
// PHYSICS KEPT HONEST: the factor multiplies the DIRECT SUN RADIANCE only —
// shadowed water gets none (a caustic is refracted sunlight), ambient/IBL and
// point lights (the diver's lamp) are untouched — and it fades exponentially
// with depth below the surface, so the shallows dance while the deep stays
// moody. Fragments at/above the waterline return exactly 1.0, and the whole
// path is gated on the uniform flag, so dry land is byte-identical.
// ===========================================================================
float causticWeb(vec2 p, float t) {
    // Three interfering directional waves; the ridge field peaks (1.0) where
    // the sum crosses zero — a slowly-boiling cellular web, no texture fetch.
    float s = sin(dot(p, vec2( 1.00,  0.31)) + t * 1.07)
            + sin(dot(p, vec2(-0.44,  0.87)) * 1.31 - t * 1.31)
            + sin(dot(p, vec2( 0.53, -0.85)) * 1.73 + t * 0.83);
    return 1.0 - abs(s) * (1.0 / 3.0);
}
float causticMod(vec3 wp) {
    float depth = ssao.caustics.y - wp.y;      // meters below the local surface
    if (depth <= 0.0) return 1.0;              // dry / above the waterline
    float t = ssao.caustics.z;
    // Layer A coarse + layer B finer (~2.4 m / ~0.9 m cells), drifting in
    // different directions at different speeds. min() -> filaments.
    float a = causticWeb(wp.xz * 2.6 + vec2( t * 0.13, t * 0.06), t);
    float b = causticWeb(wp.xz * 4.3 + vec2(-t * 0.09, t * 0.11), t * 1.23 + 4.2);
    float fil = pow(clamp(min(a, b), 0.0, 1.0), 3.0);
    // Strongest just under the surface, gone by ~12-15 m: physically right
    // (the wave-lens focus lives in the shallows) and it keeps the deep moody.
    float fade = exp2(-depth * 0.30) * clamp(ssao.caustics.w, 0.0, 1.0);
    // 0.55 trough / 2.95 crest around unity: bright dancing filaments over a
    // gently dimmed floor, mean close enough to 1 that the bed's overall
    // exposure doesn't jump when the effect engages.
    return mix(1.0, 0.55 + 2.4 * fil, fade);
}
#endif  // X3_MESH_CAUSTICS_GLSL
