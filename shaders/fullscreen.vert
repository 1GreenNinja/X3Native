#version 450

// Full-screen-triangle vertex shader for the HDR post stack (bloom + composite).
//
// CLEAN-ROOM, original: a vertexless oversized triangle generated from
// gl_VertexIndex covers the whole framebuffer (the standard "big triangle"
// trick). It emits a [0,1] UV for the post passes to sample the source image,
// and writes depth at the near plane (no depth test in the post pipelines).
// No VBO / vertex input state is bound; the post passes draw 3 vertices.

layout(location = 0) out vec2 vUV;

void main() {
    // (-1,-1)->(0,0), (3,-1)->(2,0), (-1,3)->(0,2): covers [-1,1]^2 once, with
    // the UV running 0..1 across the visible region (Vulkan: UV.y top-to-bottom
    // matches the framebuffer because we map (clip.xy*0.5+0.5) directly).
    vec2 p = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                  (gl_VertexIndex == 2) ? 3.0 : -1.0);
    vUV = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
