#version 450

// Salvari crystal-matrix WORMHOLE -- fragment stage (SOURCE-OF-TRUTH for the
// shader-path). The SHIPPED VFX renders a baked crystal-matrix texture wrapped on
// a faceted, inside-out tube via drawMeshEmissive (app/space/wormhole_vfx.cpp),
// because IRenderDevice hides custom pipeline creation. This shader expresses the
// SAME per-pixel crystal-matrix formula (faceted blue->purple->white energy
// tunnel) so that when an RHI lane lands custom-pipeline plumbing the effect can
// switch to a true per-pixel march without re-deriving the look. It is compiled
// to SPIR-V by the build (validated by glslc) but not yet bound.
//
// Look: project the pixel into tunnel space (angle theta around the axis, depth
// along the axis), then blend a blue WALL, PURPLE prismatic glints at facet
// seams, energy STREAKS that scroll with time toward the camera, and a WHITE-HOT
// convergence point ahead whose intensity rises with `progress`.

layout(set = 0, binding = 0) uniform WormholeUBO {
    mat4 invViewProj;   // unproject centered-NDC -> world ray (shader-path)
    vec4 params0;       // x = radius, y = length, z = flowSpeed, w = facetDensity
    vec4 params1;       // xyz = wallColor,   w = timeSec
    vec4 params2;       // xyz = accentColor, w = progress (0..1)
    vec4 params3;       // xyz = coreColor,   w = unused
} wh;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

const float PI  = 3.14159265359;
const float TAU = 6.28318530718;

float hash1(float x) {
    float p = fract(x * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

void main() {
    float flowSpeed    = max(wh.params0.z, 0.0);
    float facetDensity = max(wh.params0.w, 3.0);
    vec3  wallColor    = wh.params1.xyz;
    float timeSec      = wh.params1.w;
    vec3  accentColor  = wh.params2.xyz;
    float progress     = clamp(wh.params2.w, 0.0, 1.0);
    vec3  coreColor    = wh.params3.xyz;

    // Tunnel-space coords from screen NDC: angle around the axis + a radial depth
    // proxy (1/r style) so the convergence point sits dead-ahead and the walls
    // sweep past at the edges -- the classic fly-down-a-tunnel projection.
    vec2 uv    = vNdc;
    float theta = atan(uv.y, uv.x);                 // -PI..PI around the ring
    float rad   = max(length(uv), 1e-3);
    float depth = 1.0 / rad;                        // far center -> large depth

    // Scroll the depth toward the camera over time (energy racing past us).
    float zNorm = fract(depth * 0.15 - timeSec * flowSpeed * 0.05);

    // Facet seams around the ring -> purple prismatic glints.
    float facetF = (theta / TAU + 0.5) * facetDensity;
    float seam   = abs(fract(facetF) - 0.5) * 2.0;
    float glint  = pow(seam, 6.0);
    float jit    = hash1(floor(facetF) * 1.37);

    // Energy streaks along the axis (scrolling).
    float streak = 0.5 + 0.5 * sin(zNorm * TAU * 6.0 + facetF * 0.7);
    streak = pow(streak, 3.0);

    // White-hot convergence: bright dead-ahead (small rad), rising with progress.
    float conv = smoothstep(0.45, 0.0, rad) * (0.4 + 1.6 * progress);

    float wall = 0.35 + 0.65 * streak;
    vec3 col = wallColor * wall;
    col += accentColor * (glint * (0.6 + 0.4 * jit));
    col += coreColor * (conv + 0.5 * streak * conv);

    outColor = vec4(col, 1.0);
}
