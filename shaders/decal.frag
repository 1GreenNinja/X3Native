#version 450

// Impact-decal fragment shader (bullet holes / scorch marks — combat juice).
//
// CLEAN-ROOM, original work. A small PROCEDURAL mark (no texture asset needed):
// a dark scorched core with a soft sooty ring that fades to the rim, modulated by
// the per-instance color/opacity (the CPU fades A over the decal's lifetime). The
// decal sits in LINEAR HDR; the shared ACES tonemap in composite.frag maps it.

layout(location = 0) in vec4 vColor;   // linear rgb, a = opacity (CPU lifetime fade)
layout(location = 1) in vec2 vUv;      // [0,1] quad uv

layout(location = 0) out vec4 outColor;

void main() {
    vec2 c = vUv * 2.0 - 1.0;          // [-1,1]
    float r = length(c);
    if (r > 1.0) discard;             // round mark

    // Procedural scorch: a darker, denser core that thins toward the rim, plus a
    // faint sooty halo so the edge isn't a hard circle.
    float core = 1.0 - smoothstep(0.0, 0.55, r);   // 1 in the center -> 0 by 0.55
    float halo = 1.0 - smoothstep(0.45, 1.0, r);   // soft fade to the rim
    float mark = clamp(core * 0.85 + halo * 0.35, 0.0, 1.0);

    float alpha = vColor.a * mark;
    if (alpha <= 0.004) discard;

    // Scorch marks are dark: the rgb is the (already-dark) decal color; opacity
    // drives the alpha blend so the surface beneath shows through at the rim.
    outColor = vec4(vColor.rgb, alpha);
}
