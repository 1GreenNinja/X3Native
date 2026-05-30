#version 450

// Salvari crystal-matrix WORMHOLE -- vertex stage (SOURCE-OF-TRUTH for the
// shader-path; the shipped VFX uses the baked-texture + faceted-tube path via
// drawMeshEmissive, see app/space/wormhole_vfx.cpp, because the RHI hides custom
// pipeline creation). This stage emits a full-screen triangle and passes the
// centered-NDC coordinate to the fragment stage so the frag can march a tunnel
// per pixel (the per-pixel formula lives in wormhole.frag).

layout(location = 0) out vec2 vNdc;

void main() {
    vec2 p;
    if (gl_VertexIndex == 0)      p = vec2(-1.0, -1.0);
    else if (gl_VertexIndex == 1) p = vec2( 3.0, -1.0);
    else                          p = vec2(-1.0,  3.0);
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);
}
