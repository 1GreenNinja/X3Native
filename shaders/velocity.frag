#version 450

// PER-OBJECT SCREEN-SPACE VELOCITY pre-pass fragment shader (#4 velocity buffer).
//
// Writes (prevUV - curUV) into an RG16F target: the per-pixel screen-space
// motion vector in [0,1] UV space (so the TAA resolve adds it straight to the
// pixel UV to find the history sample). Jitter is removed from BOTH endpoints
// here so the vector is true surface motion, not the per-frame jitter wobble.
//
// curClip / prevClip arrive UNJITTERED from the vertex stage (computed with the
// unjittered viewProjs). The remaining jitter terms vu.jitter.xy (current) and
// vu.jitter.zw (previous) are the per-frame NDC offsets the camera projection
// added; we subtract them from the respective NDC so the MV is jitter-free.

layout(set = 0, binding = 4) uniform VelUBO {
    mat4 viewProjCurUnjit;
    mat4 viewProjPrevUnjit;
    vec4 jitter;             // xy = current jitter (NDC), zw = previous jitter (NDC)
} vu;

layout(location = 0) in vec4 vCurClip;
layout(location = 1) in vec4 vPrevClip;

layout(location = 0) out vec2 outVelocity;  // prevUV - curUV (UV space)

void main() {
    // The unjittered matrices already exclude jitter, so the jitter lanes are
    // 0 by construction here; subtracted explicitly to stay correct if a caller
    // ever feeds jittered matrices instead (defensive, costs nothing).
    vec2 curNdc  = vCurClip.xy  / max(abs(vCurClip.w),  1e-8) * sign(vCurClip.w)  - vu.jitter.xy;
    vec2 prevNdc = vPrevClip.xy / max(abs(vPrevClip.w), 1e-8) * sign(vPrevClip.w) - vu.jitter.zw;

    vec2 curUV  = curNdc  * 0.5 + 0.5;
    vec2 prevUV = prevNdc * 0.5 + 0.5;

    outVelocity = prevUV - curUV;
}
