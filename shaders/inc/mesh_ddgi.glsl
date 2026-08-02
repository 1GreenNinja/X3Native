#ifndef X3_MESH_DDGI_GLSL
#define X3_MESH_DDGI_GLSL
// ===========================================================================
// DDGI probe-field sample (r_ddgi) — classic Majercik et al. 2019 weighting.
// Trilinear over the 8 surrounding probes, each weight shaped by:
//   * a soft BACKFACE term (probes behind the surface contribute little),
//   * the CHEBYSHEV visibility test against the probe's mean/mean^2 depth
//     (statistical occlusion — the no-leak part: a probe across a wall sees
//     a much shorter mean distance in this direction than the fragment's
//     actual distance, so its weight collapses),
//   * a self-shadow BIAS (the sample point is nudged along normal + view so
//     surface-adjacent rays don't read their own wall).
// Returns rgb = DDGI irradiance (same E units as irradianceCube), a = grid
// CONFIDENCE in [0,1] (1 well inside the probe volume, fading to 0 at/outside
// its boundary — the caller lerps from the IBL/flat ambient by this).
// ===========================================================================
vec2 ddgiSignNotZero(vec2 v) { return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 ddgiOctEncode(vec3 v) {
    v /= (abs(v.x) + abs(v.y) + abs(v.z));
    vec2 e = v.xy;
    if (v.z < 0.0) e = (1.0 - abs(e.yx)) * ddgiSignNotZero(e);
    return e;                                       // [-1,1]^2
}
// Probe tile UV inside an atlas: tileU = px + py*countX, tileV = pz; T texels
// interior + 1px border each side. Matches ddgi_update.comp's layout exactly.
vec2 ddgiAtlasUV(ivec3 p, vec3 dir, float T) {
    float tile = T + 2.0;
    vec3 counts = ssao.ddgiCounts.xyz;
    vec2 tileBase = vec2(float(p.x) + float(p.y) * counts.x, float(p.z)) * tile;
    vec2 oct = ddgiOctEncode(dir) * 0.5 + 0.5;
    vec2 texel = tileBase + 1.0 + oct * T;
    vec2 atlasSize = vec2(counts.x * counts.y * tile, counts.z * tile);
    return texel / atlasSize;
}
vec4 sampleDdgi(vec3 P, vec3 N, vec3 V) {
    vec3 origin  = ssao.ddgiOrigin.xyz;
    vec3 spacing = max(ssao.ddgiSpacing.xyz, vec3(1e-3));
    vec3 counts  = ssao.ddgiCounts.xyz;
    float visMaxDist = ssao.ddgiOrigin.w;

    // Self-shadow bias: nudge the sample point off the surface toward the
    // viewer so probe rays that stopped ON this wall don't occlude it.
    float minSpacing = min(spacing.x, min(spacing.y, spacing.z));
    vec3  biasVec = (N * 0.6 + V * 0.4) * (minSpacing * 0.25 * ssao.ddgiCtrl.w);
    vec3  Pb = P + biasVec;

    vec3 g = (Pb - origin) / spacing;
    vec3 baseF = floor(g);
    vec3 alpha = clamp(g - baseF, 0.0, 1.0);
    ivec3 base = ivec3(baseF);
    ivec3 maxC = ivec3(counts) - 1;

    // Grid confidence: 1 inside, fades to 0 over the outermost half-cell.
    vec3 gc = (P - origin) / spacing;
    vec3 edge = min(gc, counts - 1.0 - gc);        // cells to the nearest face
    float contain = clamp(min(edge.x, min(edge.y, edge.z)) * 2.0 + 1.0, 0.0, 1.0);
    if (contain <= 0.0) return vec4(0.0);

    vec3 sumIrr = vec3(0.0);
    float sumW = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 pc = clamp(base + o, ivec3(0), maxC);
        vec3 probePos = origin + vec3(pc) * spacing;

        vec3 toProbe = probePos - Pb;
        float distToProbe = max(length(toProbe), 1e-4);
        vec3 dirToProbe = toProbe / distToProbe;

        // Trilinear weight (from the UNclamped cell alpha).
        vec3 tri = mix(1.0 - alpha, alpha, vec3(o));
        float w = max(tri.x * tri.y * tri.z, 1e-5);

        // Soft backface (Majercik): smooth, never fully zero.
        float wn = (dot(dirToProbe, N) + 1.0) * 0.5;
        w *= wn * wn + 0.2;

        // Chebyshev visibility from the probe's depth moments along the
        // probe->point direction.
        vec2 mm = texture(ddgiVisTex, ddgiAtlasUV(pc, -dirToProbe, 14.0)).rg;
        float mean = mm.x;
        float r = min(distToProbe, visMaxDist);
        if (r > mean) {
            float variance = abs(mm.y - mm.x * mm.x) + 1e-4;
            float d = r - mean;
            float cheb = variance / (variance + d * d);
            w *= max(cheb * cheb * cheb, 0.0);
        }
        w = max(w, 1e-6);

        sumIrr += texture(ddgiIrrTex, ddgiAtlasUV(pc, N, 6.0)).rgb * w;
        sumW += w;
    }
    return vec4(sumIrr / max(sumW, 1e-5), contain);
}
#endif  // X3_MESH_DDGI_GLSL
