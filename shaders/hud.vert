#version 450

// 2D screen-space HUD pipeline. Vertices arrive already transformed to NDC on the
// CPU (from pixel coords + framebuffer size), with per-vertex UV and RGBA color so
// solid quads (white texel) and text glyphs (font atlas) batch in one stream.
layout(location = 0) in vec2 inPosNDC;   // clip-space xy, z=0, w=1
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    gl_Position = vec4(inPosNDC, 0.0, 1.0);
    vUV = inUV;
    vColor = inColor;
}
