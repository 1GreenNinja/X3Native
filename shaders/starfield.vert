#version 450

// Procedural starfield — vertex stage. Emits a full-screen triangle that covers
// the viewport regardless of viewport size, with the centered-NDC coordinate
// passed to the fragment shader (in [-1,1]) so the frag can unproject a
// world-space view-direction per pixel for the view-direction hash.
//
// Two CCW vertices per triangle, three vertices total. With VK_CULL_MODE_NONE
// (or VK_CULL_MODE_BACK_BIT + CCW front-face) the triangle covers the screen.

// Full-screen triangle pattern (no vertex buffer needed): the three corners
// (-1,-1), (3,-1), (-1,3) cover the entire [-1,1] NDC square exactly once.

layout(location = 0) out vec2 vNdc;

void main() {
    // gl_VertexIndex in {0,1,2} -> the three triangle corners.
    vec2 p;
    if (gl_VertexIndex == 0)      p = vec2(-1.0, -1.0);
    else if (gl_VertexIndex == 1) p = vec2( 3.0, -1.0);
    else                          p = vec2(-1.0,  3.0);
    vNdc = p;
    // Draw at the FAR plane (z = 1.0 in NDC) so any subsequent opaque draws
    // with depth-test LEQUAL overdraw the starfield. The starfield itself
    // must be drawn FIRST or with depth-test disabled.
    gl_Position = vec4(p, 1.0, 1.0);
}
