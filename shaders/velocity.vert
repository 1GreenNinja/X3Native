#version 450

// PER-OBJECT SCREEN-SPACE VELOCITY pre-pass vertex shader (#4 velocity buffer).
//
// CLEAN-ROOM, original. Runs right after the depth pre-pass, re-rasterizing the
// SAME opaque geometry (same per-object SSBO + indirect draws, same EQUAL depth)
// but writing a two-channel screen-space MOTION VECTOR instead of color. The
// motion vector is (prevUV - curUV): where this surface was last frame minus
// where it is now, in [0,1] UV space. The TAA resolve reprojects history with
// it, so fast DYNAMIC and SKINNED objects no longer rely on the neighborhood
// clamp alone (which traded their ghosting for shimmer).
//
// JITTER HANDLING (correct MV): TAA folds a sub-pixel Halton jitter into the
// camera projection so EVERY raster pass (incl. this one's depth EQUAL match)
// rasters the jittered frame. But the motion vector must be UNJITTERED — it
// describes true surface motion, not the per-frame jitter wobble. So:
//   * gl_Position uses the JITTERED viewProj (cam UBO) — matches the depth buffer
//     so the EQUAL test keeps exactly the depth-prepass fragments.
//   * curClip/prevClip (the values the MV is computed from) use the UNJITTERED
//     current + previous viewProj (from the velocity UBO). curJit/prevJit jitter
//     is removed analytically in the fragment stage.
//
// SKINNED geometry: the skinning compute writes this frame's deformed vertices
// into dynVbo[frameIdx] and the PREVIOUS frame's into dynVbo[prevSlot]. The
// velocity pass binds dynVbo[frameIdx] as the vertex buffer (inPos = current
// skinned pos) and dynVbo[prevSlot] as a second stream (inPrevPos = previous
// skinned pos), so per-vertex skinning deformation is captured, not just the
// instance transform. For STATIC meshes both streams point at the same VBO
// (inPrevPos == inPos), so the skinning term vanishes and only model/camera
// motion remains. The C++ side picks the right buffers per draw.

// MUST match the std430 stride of mesh.vert's ObjectData (160 B) AND the C++
// ObjectData. Only `model` is read here; trailing fields force the stride.
struct ObjectData {
    mat4 model;           // 64
    vec4 baseColorFactor; // 16
    vec4 emissive;        // 16
    uint texIndex;        // 96
    uint flags;
    uint _pad1;
    uint _pad2;
    uint normalTexIndex;  // 112
    uint mrTexIndex;
    uint emissiveTexIndex;
    uint detailPacked;
    vec4 glassParams;     // 128
    vec4 glassTint;       // 144 -> 160
};

layout(std430, set = 0, binding = 0) readonly buffer Objects {
    ObjectData objects[];
} objBuf;

// PREVIOUS-frame per-object model matrices (one mat4 row per object SSBO row,
// same indexing as objBuf). Filled by the CPU each frame from last frame's
// transforms. For a brand-new object (no history) the CPU seeds prev = current
// so the object's first frame reports zero object motion (camera-only).
layout(std430, set = 0, binding = 3) readonly buffer PrevObjects {
    mat4 prevModel[];
} prevBuf;

// D15 GPU cull indirection (identity when cull off) — same as depth.vert.
layout(std430, set = 0, binding = 2) readonly buffer VisibleIdx {
    uint idx[];
} visBuf;

// Camera UBO (set0/b1) — JITTERED viewProj (matches the depth buffer).
layout(set = 0, binding = 1) uniform Camera {
    mat4 viewProj;        // JITTERED current viewProj
    mat4 lightViewProj;
} cam;

// Velocity UBO (set0/b4): the UNJITTERED current + previous viewProj used to
// compute the motion vector, plus the two frames' jitter offsets (NDC) so the
// fragment stage can analytically remove jitter from each endpoint.
layout(set = 0, binding = 4) uniform VelUBO {
    mat4 viewProjCurUnjit;   // world -> current UNJITTERED clip
    mat4 viewProjPrevUnjit;  // world -> previous UNJITTERED clip
    vec4 jitter;             // xy = current jitter (NDC), zw = previous jitter (NDC)
} vu;

layout(location = 0) in vec3 inPos;       // current (skinned) position
layout(location = 1) in vec3 inNormal;    // unused; keeps the MeshVertex layout
layout(location = 2) in vec2 inUV;        // unused
layout(location = 3) in vec3 inPrevPos;   // previous-frame (skinned) position; == inPos for static

layout(location = 0) out vec4 vCurClip;   // UNJITTERED current clip
layout(location = 1) out vec4 vPrevClip;  // UNJITTERED previous clip

void main() {
    uint row = visBuf.idx[gl_InstanceIndex];
    ObjectData o = objBuf.objects[row];
    mat4 prevM  = prevBuf.prevModel[row];

    vec4 worldCur  = o.model * vec4(inPos, 1.0);
    vec4 worldPrev = prevM   * vec4(inPrevPos, 1.0);

    // Rasterize with the JITTERED viewProj so the depth EQUAL test keeps exactly
    // the same fragments the depth pre-pass wrote.
    gl_Position = cam.viewProj * worldCur;

    // Motion-vector endpoints use the UNJITTERED matrices.
    vCurClip  = vu.viewProjCurUnjit  * worldCur;
    vPrevClip = vu.viewProjPrevUnjit * worldPrev;
}
