#version 450

// Analytic-sky vertex shader (open-world track, task A).
//
// A vertexless full-screen triangle: three verts generated from gl_VertexIndex
// (no VBO bound, no vertex input state) cover the whole framebuffer. The clip
// position is emitted at the FAR plane (z = w  =>  post-divide depth == 1.0) so
// the sky pipeline can run with depthTest = LESS_OR_EQUAL / depthWrite = OFF:
// it passes the test only where the cleared depth (1.0) still stands, i.e. where
// no opaque geometry has written a nearer depth. Geometry therefore occludes the
// sky for free, and the sky writes nothing to depth so later passes are unharmed.
//
// The fragment stage needs a per-pixel world-space view ray; we forward the NDC
// xy and let the fragment shader unproject it with the camera's inverse viewProj
// (passed in the sky UBO). No engine geometry, no per-object data.

layout(location = 0) out vec2 vNdc;   // [-1,1] clip-space xy for ray reconstruction

void main() {
    // Oversized triangle covering the screen: (-1,-1), (3,-1), (-1,3).
    vec2 p = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                  (gl_VertexIndex == 2) ? 3.0 : -1.0);
    vNdc = p;
    // z = w (= 1.0) puts the post-perspective depth at the far plane (1.0).
    gl_Position = vec4(p, 1.0, 1.0);
}
