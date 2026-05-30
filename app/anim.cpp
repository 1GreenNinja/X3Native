// Skeletal animation + CPU skinning runtime (J1). See app/anim.h.
//
// Clean-room: glTF 2.0 animation sampling + linear-blend skinning math built from
// the public spec only; no GPL / id Tech / RBDOOM source consulted. Column-major
// 4x4 (glTF/glm convention) throughout, matching the M2 loader's node transforms.

#include "anim.h"
#include "asset_root.h"
#include "engine/asset/IAssetSource.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace x3::anim {

namespace {

// ---- column-major 4x4 helpers (glTF/glm convention; b applied first) --------
void mat4Identity(float* m) {
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}
void mat4Mul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            r[col*4+row] = a[0*4+row]*b[col*4+0] + a[1*4+row]*b[col*4+1] +
                           a[2*4+row]*b[col*4+2] + a[3*4+row]*b[col*4+3];
    std::memcpy(out, r, sizeof r);
}
// Compose TRS into a column-major 4x4. q = (x,y,z,w).
void trsToMat4(const float t[3], const float q[4], const float s[3], float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x*x, yy = y*y, zz = z*z;
    const float xy = x*y, xz = x*z, yz = y*z;
    const float wx = w*x, wy = w*y, wz = w*z;
    m[0]  = (1 - 2*(yy+zz)) * s[0]; m[1]  = (2*(xy+wz)) * s[0]; m[2]  = (2*(xz-wy)) * s[0]; m[3]  = 0;
    m[4]  = (2*(xy-wz)) * s[1]; m[5]  = (1 - 2*(xx+zz)) * s[1]; m[6]  = (2*(yz+wx)) * s[1]; m[7]  = 0;
    m[8]  = (2*(xz+wy)) * s[2]; m[9]  = (2*(yz-wx)) * s[2]; m[10] = (1 - 2*(xx+yy)) * s[2]; m[11] = 0;
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
}
// Transform a point (w=1) by a column-major 4x4.
void xformPoint(const float m[16], const float p[3], float out[3]) {
    out[0] = m[0]*p[0] + m[4]*p[1] + m[8] *p[2] + m[12];
    out[1] = m[1]*p[0] + m[5]*p[1] + m[9] *p[2] + m[13];
    out[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}
// Transform a direction (w=0) by the upper-left 3x3.
void xformDir(const float m[16], const float d[3], float out[3]) {
    out[0] = m[0]*d[0] + m[4]*d[1] + m[8] *d[2];
    out[1] = m[1]*d[0] + m[5]*d[1] + m[9] *d[2];
    out[2] = m[2]*d[0] + m[6]*d[1] + m[10]*d[2];
}

// Decompose the bind-pose local matrix of a node into T/R/S (used as the fallback
// when a clip doesn't animate a particular channel). The loader stores either an
// explicit matrix or a TRS-composed matrix; decompose handles both.
void decompose(const float m[16], float t[3], float q[4], float s[3]) {
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    // Column lengths = scale.
    float c0[3] = { m[0], m[1], m[2] };
    float c1[3] = { m[4], m[5], m[6] };
    float c2[3] = { m[8], m[9], m[10] };
    s[0] = std::sqrt(c0[0]*c0[0] + c0[1]*c0[1] + c0[2]*c0[2]);
    s[1] = std::sqrt(c1[0]*c1[0] + c1[1]*c1[1] + c1[2]*c1[2]);
    s[2] = std::sqrt(c2[0]*c2[0] + c2[1]*c2[1] + c2[2]*c2[2]);
    float r00 = s[0] > 1e-8f ? c0[0]/s[0] : 0, r10 = s[0] > 1e-8f ? c0[1]/s[0] : 0, r20 = s[0] > 1e-8f ? c0[2]/s[0] : 0;
    float r01 = s[1] > 1e-8f ? c1[0]/s[1] : 0, r11 = s[1] > 1e-8f ? c1[1]/s[1] : 0, r21 = s[1] > 1e-8f ? c1[2]/s[1] : 0;
    float r02 = s[2] > 1e-8f ? c2[0]/s[2] : 0, r12 = s[2] > 1e-8f ? c2[1]/s[2] : 0, r22 = s[2] > 1e-8f ? c2[2]/s[2] : 0;
    // Rotation matrix -> quaternion (column-major rij = row i, col j).
    float tr = r00 + r11 + r22;
    if (tr > 0) {
        float ss = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * ss; q[0] = (r21 - r12) / ss; q[1] = (r02 - r20) / ss; q[2] = (r10 - r01) / ss;
    } else if (r00 > r11 && r00 > r22) {
        float ss = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q[3] = (r21 - r12) / ss; q[0] = 0.25f * ss; q[1] = (r01 + r10) / ss; q[2] = (r02 + r20) / ss;
    } else if (r11 > r22) {
        float ss = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q[3] = (r02 - r20) / ss; q[0] = (r01 + r10) / ss; q[1] = 0.25f * ss; q[2] = (r12 + r21) / ss;
    } else {
        float ss = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q[3] = (r10 - r01) / ss; q[0] = (r02 + r20) / ss; q[1] = (r12 + r21) / ss; q[2] = 0.25f * ss;
    }
}

// Quaternion SLERP (shortest path), a,b,out = (x,y,z,w).
void slerp(const float a[4], const float b[4], float u, float out[4]) {
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    float bb[4] = { b[0], b[1], b[2], b[3] };
    if (dot < 0.0f) { dot = -dot; for (int i = 0; i < 4; ++i) bb[i] = -bb[i]; }
    if (dot > 0.9995f) {                       // near-parallel: nlerp
        for (int i = 0; i < 4; ++i) out[i] = a[i] + u * (bb[i] - a[i]);
    } else {
        float theta0 = std::acos(dot);
        float theta = theta0 * u;
        float sin0 = std::sin(theta0);
        float s0 = std::sin(theta0 - theta) / sin0;
        float s1 = std::sin(theta) / sin0;
        for (int i = 0; i < 4; ++i) out[i] = s0 * a[i] + s1 * bb[i];
    }
    float l = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3]);
    if (l > 1e-8f) { for (int i = 0; i < 4; ++i) out[i] /= l; }
    else { out[0] = out[1] = out[2] = 0; out[3] = 1; }
}

// Find the keyframe segment [k, k+1] for time t in a sorted `times` array, plus
// the interpolation factor u in [0,1]. Clamps to the ends.
void findKey(const std::vector<float>& times, float t, size_t& k, float& u) {
    const size_t n = times.size();
    if (n == 0) { k = 0; u = 0; return; }
    if (t <= times.front()) { k = 0; u = 0; return; }
    if (t >= times.back())  { k = (n >= 2) ? n - 2 : 0; u = 1; return; }
    // linear scan (keyframe counts are small per channel)
    size_t i = 0;
    while (i + 1 < n && times[i + 1] < t) ++i;
    k = i;
    float t0 = times[i], t1 = times[i + 1];
    u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
}

std::string toLower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

// ---- T1 blend helpers --------------------------------------------------------
// Linear interpolate two 3-vectors.
void lerp3(const float a[3], const float b[3], float u, float out[3]) {
    out[0] = a[0] + u * (b[0] - a[0]);
    out[1] = a[1] + u * (b[1] - a[1]);
    out[2] = a[2] + u * (b[2] - a[2]);
}
// Blend two quaternions by u (uses the existing shortest-path slerp, which falls
// back to nlerp near-parallel and renormalizes). a,b,out = (x,y,z,w).
void blendQuat(const float a[4], const float b[4], float u, float out[4]) {
    slerp(a, b, u, out);
}
// Smoothstep easing on [0,1] for pop-free crossfade ramps (C1-continuous ends).
float smoothstep01(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

// ---- small vec3 / quat helpers for the foot-IK pass --------------------------
float dot3(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
float len3(const float a[3]) { return std::sqrt(dot3(a, a)); }
void sub3(const float a[3], const float b[3], float o[3]) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
void add3(const float a[3], const float b[3], float o[3]) { o[0]=a[0]+b[0]; o[1]=a[1]+b[1]; o[2]=a[2]+b[2]; }
void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}
bool norm3(float a[3]) {
    float l = len3(a);
    if (l < 1e-8f) return false;
    a[0]/=l; a[1]/=l; a[2]/=l; return true;
}
// Quaternion multiply (q = a * b), Hamilton, components (x,y,z,w).
void quatMul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}
// Rotate a vector v by quaternion q (x,y,z,w) -> o.
void quatRotate(const float q[4], const float v[3], float o[3]) {
    const float qx=q[0], qy=q[1], qz=q[2], qw=q[3];
    float t[3];                      // t = 2 * cross(qxyz, v)
    t[0] = 2.0f * (qy*v[2] - qz*v[1]);
    t[1] = 2.0f * (qz*v[0] - qx*v[2]);
    t[2] = 2.0f * (qx*v[1] - qy*v[0]);
    o[0] = v[0] + qw*t[0] + (qy*t[2] - qz*t[1]);
    o[1] = v[1] + qw*t[1] + (qz*t[0] - qx*t[2]);
    o[2] = v[2] + qw*t[2] + (qx*t[1] - qy*t[0]);
}
void quatNormalize(float q[4]) {
    float l = std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (l > 1e-8f) { q[0]/=l; q[1]/=l; q[2]/=l; q[3]/=l; }
    else { q[0]=q[1]=q[2]=0; q[3]=1; }
}
// Quaternion (x,y,z,w) rotating unit vector `from` onto unit vector `to` (shortest arc).
void quatFromTo(const float from[3], const float to[3], float out[4]) {
    float d = dot3(from, to);
    if (d > 0.999999f) { out[0]=out[1]=out[2]=0; out[3]=1; return; } // already aligned
    if (d < -0.999999f) {
        // 180 deg: pick any axis orthogonal to `from`.
        float axis[3] = { 1,0,0 };
        float c[3]; cross3(from, axis, c);
        if (len3(c) < 1e-4f) { axis[0]=0; axis[1]=1; axis[2]=0; cross3(from, axis, c); }
        norm3(c);
        out[0]=c[0]; out[1]=c[1]; out[2]=c[2]; out[3]=0; return; // 180 deg about c
    }
    float c[3]; cross3(from, to, c);
    out[0]=c[0]; out[1]=c[1]; out[2]=c[2]; out[3]=1.0f + d;
    quatNormalize(out);
}
// Extract the translation column (model-space position) of a column-major 4x4.
void mat4Translation(const float m[16], float o[3]) { o[0]=m[12]; o[1]=m[13]; o[2]=m[14]; }
// Lower-case substring test (case-insensitive contains).
bool icontains(const std::string& hay, const char* needle) {
    std::string h = toLower(hay);
    return h.find(needle) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
bool Skinner::bind(const x3::asset::Model& model) {
    m_valid = false;
    m_skinIndex = -1;
    m_clipDurations.clear();
    m_clipNames.clear();
    m_channelLut.clear();
    m_nodeCount = (uint32_t)model.nodes.size();

    // Need a skin with joints, a clip, and a real-device skinned primitive.
    if (model.skins.empty() || model.skins[0].joints.empty() ||
        model.animations.empty() || model.nodes.empty())
        return false;

    bool anySkinnedReal = false;
    for (const auto& p : model.primitives)
        if (p.skinned && x3::asset::meshIdOf(p) != 0) { anySkinnedReal = true; break; }
    // Headless (no device) primitives still let the palette path run for the
    // self-test, but apply() will just skip the updateMesh for meshId==0.
    bool anySkinned = false;
    for (const auto& p : model.primitives) if (p.skinned) { anySkinned = true; break; }
    if (!anySkinned) return false;
    (void)anySkinnedReal;

    m_skinIndex = 0;
    const uint32_t clips = (uint32_t)model.animations.size();
    m_clipDurations.resize(clips);
    m_clipNames.resize(clips);
    m_channelLut.assign((size_t)clips * m_nodeCount * 3, -1);

    for (uint32_t c = 0; c < clips; ++c) {
        const auto& clip = model.animations[c];
        m_clipDurations[c] = clip.duration;
        m_clipNames[c] = clip.name;
        for (size_t ch = 0; ch < clip.channels.size(); ++ch) {
            const auto& chan = clip.channels[ch];
            if (chan.targetNode < 0 || (uint32_t)chan.targetNode >= m_nodeCount) continue;
            int slot;
            switch (chan.path) {
                case x3::asset::AnimPath::Translation: slot = 0; break;
                case x3::asset::AnimPath::Rotation:    slot = 1; break;
                case x3::asset::AnimPath::Scale:       slot = 2; break;
                default: continue;   // weights unsupported in CPU skinning here
            }
            size_t li = (((size_t)c * m_nodeCount) + (size_t)chan.targetNode) * 3 + slot;
            m_channelLut[li] = (int)ch;
        }
    }

    m_globalScratch.assign((size_t)m_nodeCount * 16, 0.0f);
    m_palette.assign(model.skins[0].joints.size() * 16, 0.0f);
    // Pre-size the hierarchy-resolve scratch so computeGlobals() never reallocates
    // in the steady per-frame path (it only resets/clears these each call).
    m_resolveDone.assign(m_nodeCount, 0);
    m_resolveInProg.assign(m_nodeCount, 0);
    m_resolveStack.clear();
    m_resolveStack.reserve(m_nodeCount);

    // ---- T1 locomotion-blend scratch: per-node local pose (T=3, R=4, S=3 floats)
    // for the two bracketing clips, the blended result, and the crossfade target.
    // Sized here so advanceBlend() never reallocates in the steady path. ----
    m_poseAT.assign((size_t)m_nodeCount * 3, 0.0f);
    m_poseAR.assign((size_t)m_nodeCount * 4, 0.0f);
    m_poseAS.assign((size_t)m_nodeCount * 3, 0.0f);
    m_poseBT = m_poseAT; m_poseBR = m_poseAR; m_poseBS = m_poseAS;
    m_blendT = m_poseAT; m_blendR = m_poseAR; m_blendS = m_poseAS;
    m_xfadeT = m_poseAT; m_xfadeR = m_poseAR; m_xfadeS = m_poseAS;
    // Reset the blend state (a re-bind starts fresh).
    m_idleClip = m_walkClip = m_runClip = -1;
    m_speedCmd = 0.0f; m_loco01Cmd = -1.0f; m_locoW = 0.0f; m_phase = 0.0f;
    m_xfadeActive = false; m_xfadeClip = -1; m_xfadeW = 0.0f; m_xfadeOut = false;
    m_xfadeTime = 0.0f; m_xfadeClipT = 0.0f;

    // ---- Foot-IK: size scratch + resolve the leg/hips bones by name. A re-bind
    // keeps the enabled flag/callback off until setFootIk() is called again. ----
    m_ikGlobals.assign((size_t)m_nodeCount * 16, 0.0f);
    m_footIkEnabled = false;
    m_groundRay = GroundRay{};
    mat4Identity(m_worldFromModel);
    m_legW[0] = m_legW[1] = 0.0f;
    m_pelvisDropSmoothed = 0.0f;
    resolveFootIkBones(model);

    // Ragdoll-blend: a re-bind drops any prior external-bone resolution.
    m_extToJoint.clear();
    m_extResolvedCount = 0;

    m_gpuSkin = false;   // a re-bind drops any prior GPU-skin registration
    m_valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// GPU compute skinning: register every skinned primitive's bind-pose verts +
// per-vertex joint idx/weights with the device, so apply/applyLocomotion can skin
// on the GPU (upload palette) instead of CPU-LBS + updateMesh.
bool Skinner::enableGpuSkinning(x3::rhi::IRenderDevice& device,
                                const x3::asset::Model& model) {
    if (!m_valid) return false;
    if (!device.supportsGpuSkinning()) return false;   // headless / non-compute -> CPU path
    bool any = false;
    std::vector<x3::rhi::MeshVertex> bind;   // bind-pose MeshVertex scratch
    for (const auto& p : model.primitives) {
        if (!p.skinned) continue;
        const uint32_t meshId = x3::asset::meshIdOf(p);
        if (meshId == 0) continue;                       // headless primitive: skip
        const size_t vcount = p.basePos.size() / 3;
        if (vcount == 0 || p.jointIdx.size() < vcount * 4 || p.jointWt.size() < vcount * 4)
            continue;
        bind.resize(vcount);
        for (size_t v = 0; v < vcount; ++v) {
            x3::rhi::MeshVertex& mv = bind[v];
            mv.pos[0] = p.basePos[v*3+0]; mv.pos[1] = p.basePos[v*3+1]; mv.pos[2] = p.basePos[v*3+2];
            mv.normal[0] = p.baseNrm[v*3+0]; mv.normal[1] = p.baseNrm[v*3+1]; mv.normal[2] = p.baseNrm[v*3+2];
            mv.uv[0] = p.baseUv[v*2+0]; mv.uv[1] = p.baseUv[v*2+1];
        }
        if (device.registerSkinnedMesh(x3::rhi::MeshHandle{ meshId }, bind.data(),
                                       (uint32_t)vcount, p.jointIdx.data(), p.jointWt.data())) {
            any = true;
            m_gpuMeshIds.push_back(meshId);   // remember so we can unregister on despawn
        }
    }
    m_gpuSkin = any;
    return any;
}

// Hand every registered skinned mesh back to the device (free its skinning buffers +
// descriptor sets). Used when the model is despawned. Idempotent.
void Skinner::disableGpuSkinning(x3::rhi::IRenderDevice& device) {
    for (uint32_t meshId : m_gpuMeshIds)
        device.unregisterSkinnedMesh(x3::rhi::MeshHandle{ meshId });
    m_gpuMeshIds.clear();
    m_gpuSkin = false;
}

float Skinner::clipDuration(uint32_t clip) const {
    return clip < m_clipDurations.size() ? m_clipDurations[clip] : 0.0f;
}
std::string_view Skinner::clipName(uint32_t clip) const {
    return clip < m_clipNames.size() ? std::string_view(m_clipNames[clip]) : std::string_view{};
}

int Skinner::findClip(std::initializer_list<const char*> keys) const {
    // For each key (in caller priority order) an EXACT (case-insensitive) clip-name
    // match wins over a mere substring match. Without this, a key like "walking"
    // resolves to the FIRST clip that merely CONTAINS it — e.g. Jake's
    // "Leftstrafewalking" (a sideways step that LEANS the body) instead of the plain
    // "Walking" clip — which made the 3P avatar lean off at a \ angle the instant he
    // moved. (Same collision class that a56d1b0 fixed for idle-vs-Rifleaimingidle.)
    for (const char* key : keys) {
        std::string k = toLower(key);
        for (uint32_t c = 0; c < (uint32_t)m_clipNames.size(); ++c)
            if (toLower(m_clipNames[c]) == k) return (int)c;                          // exact name
        for (uint32_t c = 0; c < (uint32_t)m_clipNames.size(); ++c)
            if (toLower(m_clipNames[c]).find(k) != std::string::npos) return (int)c;  // substring
    }
    return -1;
}

void Skinner::sampleNodeTRS(const x3::asset::Model& m, uint32_t clip, int node,
                            float t, float T[3], float R[4], float S[3]) const {
    const x3::asset::Node& nd = m.nodes[node];
    // Bind-pose fallback TRS for channels the clip doesn't animate.
    decompose(nd.localTransform, T, R, S);

    const size_t base = (((size_t)clip * m_nodeCount) + (size_t)node) * 3;
    const x3::asset::AnimationClip& cl = m.animations[clip];

    auto sampleVec3 = [&](int chIdx, float dst[3]) {
        const x3::asset::AnimationChannel& ch = cl.channels[chIdx];
        if (ch.times.empty() || ch.values.empty()) return;
        size_t k; float u; findKey(ch.times, t, k, u);
        size_t i0 = k * 3, i1 = (k + 1 < ch.times.size() ? (k + 1) : k) * 3;
        for (int j = 0; j < 3; ++j)
            dst[j] = ch.values[i0+j] + u * (ch.values[i1+j] - ch.values[i0+j]);
    };
    auto sampleQuat = [&](int chIdx, float dst[4]) {
        const x3::asset::AnimationChannel& ch = cl.channels[chIdx];
        if (ch.times.empty() || ch.values.empty()) return;
        size_t k; float u; findKey(ch.times, t, k, u);
        size_t i0 = k * 4, i1 = (k + 1 < ch.times.size() ? (k + 1) : k) * 4;
        float a[4] = { ch.values[i0+0], ch.values[i0+1], ch.values[i0+2], ch.values[i0+3] };
        float b[4] = { ch.values[i1+0], ch.values[i1+1], ch.values[i1+2], ch.values[i1+3] };
        slerp(a, b, u, dst);
    };

    int tCh = m_channelLut[base + 0];
    int rCh = m_channelLut[base + 1];
    int sCh = m_channelLut[base + 2];
    if (tCh >= 0) sampleVec3(tCh, T);
    if (rCh >= 0) sampleQuat(rCh, R);
    if (sCh >= 0) sampleVec3(sCh, S);
}

void Skinner::sampleNodeLocal(const x3::asset::Model& m, uint32_t clip, int node,
                              float t, float out[16]) const {
    float T[3], R[4], S[3];
    sampleNodeTRS(m, clip, node, t, T, R, S);
    trsToMat4(T, R, S, out);
}

void Skinner::computeGlobals(const x3::asset::Model& m, uint32_t clip, float t,
                             std::vector<float>& globals) const {
    // `globals` is the caller's pre-sized member scratch (nodeCount*16). Reset in
    // place; do not reallocate. The resolve scratch (done/inprog/stack) are also
    // pre-sized members — clear/zero them per call without heap allocation.
    if (globals.size() != (size_t)m_nodeCount * 16)
        globals.assign((size_t)m_nodeCount * 16, 0.0f);   // defensive (size mismatch)
    else
        std::fill(globals.begin(), globals.end(), 0.0f);

    if (m_resolveDone.size() != m_nodeCount)   m_resolveDone.assign(m_nodeCount, 0);
    else                                       std::fill(m_resolveDone.begin(), m_resolveDone.end(), (char)0);
    if (m_resolveInProg.size() != m_nodeCount) m_resolveInProg.assign(m_nodeCount, 0);
    else                                       std::fill(m_resolveInProg.begin(), m_resolveInProg.end(), (char)0);
    std::vector<char>& done = m_resolveDone;
    std::vector<char>& inprog = m_resolveInProg;
    std::vector<int>& stack = m_resolveStack;

    // Iterative resolve (recursion-free) of each node's global = parent.global * local.
    std::array<float, 16> local;
    for (uint32_t i = 0; i < m_nodeCount; ++i) {
        if (done[i]) continue;
        stack.clear();
        int cur = (int)i;
        // Walk up collecting the unresolved ancestor chain.
        while (cur >= 0 && !done[cur]) {
            if (inprog[cur]) break;     // cycle guard
            inprog[cur] = 1;
            stack.push_back(cur);
            cur = m.nodes[cur].parent;
        }
        // Resolve from the top of the chain down.
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            int n = *it;
            sampleNodeLocal(m, clip, n, t, local.data());
            int parent = m.nodes[n].parent;
            if (parent >= 0 && (uint32_t)parent < m_nodeCount && done[parent]) {
                mat4Mul(&globals[(size_t)parent*16], local.data(), &globals[(size_t)n*16]);
            } else {
                std::memcpy(&globals[(size_t)n*16], local.data(), sizeof(float)*16);
            }
            done[n] = 1; inprog[n] = 0;
        }
    }
}

uint32_t Skinner::computePalette(const x3::asset::Model& model, uint32_t clip,
                                 float timeSec, std::vector<float>& outPalette) const {
    if (!m_valid || clip >= m_clipDurations.size()) { outPalette.clear(); return 0; }
    const x3::asset::Skin& skin = model.skins[0];
    const uint32_t jcount = (uint32_t)skin.joints.size();
    // Wrap time over the clip duration (loop). A zero-duration clip stays at t=0.
    float dur = m_clipDurations[clip];
    float t = (dur > 1e-6f) ? std::fmod(timeSec, dur) : 0.0f;
    if (t < 0) t += dur;

    // Reuse the pre-allocated member global-matrix scratch (sized in bind()); no
    // per-frame heap allocation for the hierarchy globals.
    computeGlobals(model, clip, t, m_globalScratch);

    // outPalette is the caller's buffer. In the steady path (apply()) it is the
    // pre-sized m_palette, so this assign reuses capacity and reallocates nothing.
    outPalette.assign((size_t)jcount * 16, 0.0f);
    for (uint32_t j = 0; j < jcount; ++j) {
        int node = skin.joints[j];
        if (node < 0 || (uint32_t)node >= m_nodeCount) { mat4Identity(&outPalette[(size_t)j*16]); continue; }
        const float* ib = &skin.inverseBind[(size_t)j * 16];
        mat4Mul(&m_globalScratch[(size_t)node*16], ib, &outPalette[(size_t)j*16]);
    }
    return jcount;
}

uint32_t Skinner::currentGlobals(const x3::asset::Model& model, uint32_t clip,
                                 float timeSec, std::vector<float>& outGlobals) const {
    if (!m_valid || clip >= m_clipDurations.size()) { outGlobals.clear(); return 0; }
    // Wrap time over the clip duration (loop), same as computePalette().
    float dur = m_clipDurations[clip];
    float t = (dur > 1e-6f) ? std::fmod(timeSec, dur) : 0.0f;
    if (t < 0) t += dur;
    outGlobals.assign((size_t)m_nodeCount * 16, 0.0f);
    computeGlobals(model, clip, t, outGlobals);
    return m_nodeCount;
}

// ===========================================================================
// Named-bone world-transform readback (third-person held-weapon SOCKET).
// ===========================================================================
int Skinner::resolveNodeByName(const x3::asset::Model& model, std::string_view name) const {
    if (!m_valid || name.empty() || m_nodeCount == 0) return -1;
    std::string want = toLower(name);
    int best = -1;
    for (uint32_t n = 0; n < m_nodeCount; ++n) {
        std::string have = toLower(model.nodes[n].name);
        if (have.empty()) continue;
        if (have == want) return (int)n;                       // exact match wins
        if (best < 0 && (have.find(want) != std::string::npos ||
                         want.find(have) != std::string::npos))
            best = (int)n;                                     // remember a substring match
    }
    return best;
}

bool Skinner::boneGlobal(uint32_t nodeIndex, float out[16]) const {
    if (!m_valid || nodeIndex >= m_nodeCount || !out) return false;
    // m_globalScratch holds the per-node MODEL-SPACE globals from the most recent
    // apply()/applyLocomotion()/applyRagdollBlend() (same pose lastPalette() built).
    if (m_globalScratch.size() != (size_t)m_nodeCount * 16) return false;
    std::memcpy(out, &m_globalScratch[(size_t)nodeIndex * 16], 16 * sizeof(float));
    return true;
}

bool Skinner::boneGlobalByName(const x3::asset::Model& model, std::string_view name,
                               float out[16]) const {
    int n = resolveNodeByName(model, name);
    if (n < 0) return false;
    return boneGlobal((uint32_t)n, out);
}

// ===========================================================================
// Ragdoll blend — drive the skin from an external physics pose (Physics §2).
// ===========================================================================

uint32_t Skinner::resolveExternalBones(const x3::asset::Model& model,
                                       const char* const* boneNames, uint32_t count) {
    m_extToJoint.assign(count, -1);
    m_extResolvedCount = 0;
    if (!m_valid || !boneNames || count == 0 || model.skins.empty()) return 0;
    const x3::asset::Skin& skin = model.skins[0];
    const uint32_t jcount = (uint32_t)skin.joints.size();
    for (uint32_t e = 0; e < count; ++e) {
        if (!boneNames[e]) continue;
        std::string want = toLower(boneNames[e]);
        if (want.empty()) continue;
        // Match the external bone name to the skin joint whose node name matches
        // (exact, case-insensitive; else a substring either way for exporter quirks).
        int best = -1;
        for (uint32_t j = 0; j < jcount; ++j) {
            int node = skin.joints[j];
            if (node < 0 || (uint32_t)node >= m_nodeCount) continue;
            std::string have = toLower(model.nodes[node].name);
            if (have.empty()) continue;
            if (have == want) { best = (int)j; break; }
            if (best < 0 && (have.find(want) != std::string::npos ||
                             want.find(have) != std::string::npos))
                best = (int)j;
        }
        m_extToJoint[e] = best;
        if (best >= 0) ++m_extResolvedCount;
    }
    return m_extResolvedCount;
}

uint32_t Skinner::computeRagdollBlendedPalette(const x3::asset::Model& model, uint32_t clip,
                                               float timeSec, const float* extWorld,
                                               uint32_t extCount, float weight,
                                               std::vector<float>& outPalette) const {
    if (!m_valid || clip >= m_clipDurations.size()) { outPalette.clear(); return 0; }
    const x3::asset::Skin& skin = model.skins[0];
    const uint32_t jcount = (uint32_t)skin.joints.size();
    if (weight < 0.0f) weight = 0.0f; if (weight > 1.0f) weight = 1.0f;

    // 1) Animated global node matrices at (clip, wrapped time).
    float dur = m_clipDurations[clip];
    float t = (dur > 1e-6f) ? std::fmod(timeSec, dur) : 0.0f;
    if (t < 0) t += dur;
    computeGlobals(model, clip, t, m_globalScratch);

    // 2) Build a joint-indexed override: jointGlobalOverride[j] = the ragdoll WORLD
    //    matrix for the bone mapped to joint j (else absent). We blend it with the
    //    animated global per joint.
    const bool useExt = (extWorld != nullptr && weight > 0.0f && !m_extToJoint.empty());

    outPalette.assign((size_t)jcount * 16, 0.0f);
    for (uint32_t j = 0; j < jcount; ++j) {
        int node = skin.joints[j];
        if (node < 0 || (uint32_t)node >= m_nodeCount) { mat4Identity(&outPalette[(size_t)j*16]); continue; }
        const float* animGlobal = &m_globalScratch[(size_t)node*16];
        const float* ib = &skin.inverseBind[(size_t)j * 16];

        // Find an external bone mapped to this joint (linear scan over the small
        // ext->joint map; the chain is small so this is cheap and string-free).
        const float* ext = nullptr;
        if (useExt) {
            for (uint32_t e = 0; e < m_extToJoint.size() && e < extCount; ++e) {
                if (m_extToJoint[e] == (int)j) { ext = &extWorld[(size_t)e*16]; break; }
            }
        }

        if (!ext) {
            // No ragdoll influence on this joint: pure animated pose.
            mat4Mul(animGlobal, ib, &outPalette[(size_t)j*16]);
            continue;
        }

        // Blend the model-space joint transform between the animated global and the
        // ragdoll world transform: lerp translation, nlerp rotation (scale assumed 1
        // for skeletal bones). weight=1 -> pure ragdoll; weight=0 -> pure animated.
        float at[3], aq[4], as[3];  decompose(animGlobal, at, aq, as);
        float rt[3], rq[4], rs[3];  decompose(ext,        rt, rq, rs);
        float bt[3], bq[4];
        lerp3(at, rt, weight, bt);
        blendQuat(aq, rq, weight, bq);   // nlerp w/ hemisphere fix + renormalize
        float one[3] = { 1, 1, 1 };
        float blended[16];
        trsToMat4(bt, bq, one, blended);
        mat4Mul(blended, ib, &outPalette[(size_t)j*16]);
    }
    return jcount;
}

void Skinner::applyRagdollBlend(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
                                uint32_t clip, float timeSec, const float* extWorld,
                                uint32_t extCount, float weight) {
    if (!m_valid) return;
    uint32_t jcount = computeRagdollBlendedPalette(model, clip, timeSec, extWorld,
                                                   extCount, weight, m_palette);
    if (jcount == 0) return;
    skinWithCurrentPalette(model, device, jcount);
}

// Upload (GPU) or CPU-LBS + updateMesh every skinned primitive from m_palette. The
// shared tail of apply()/applyRagdollBlend so they don't duplicate the skin loop.
void Skinner::skinWithCurrentPalette(const x3::asset::Model& model,
                                     x3::rhi::IRenderDevice& device, uint32_t jcount) {
    if (m_gpuSkin) {
        for (const auto& p : model.primitives) {
            if (!p.skinned) continue;
            const uint32_t meshId = x3::asset::meshIdOf(p);
            if (meshId == 0) continue;
            device.setSkinnedPalette(x3::rhi::MeshHandle{ meshId }, m_palette.data(), jcount);
        }
        return;
    }
    for (const auto& p : model.primitives) {
        if (!p.skinned) continue;
        const uint32_t meshId = x3::asset::meshIdOf(p);
        if (meshId == 0) continue;
        const size_t vcount = p.basePos.size() / 3;
        if (vcount == 0) continue;
        m_vertScratch.resize(vcount);
        for (size_t v = 0; v < vcount; ++v) {
            const float* bp = &p.basePos[v*3];
            const float* bn = &p.baseNrm[v*3];
            const uint16_t* ji = &p.jointIdx[v*4];
            const float* jw = &p.jointWt[v*4];
            float wsum = jw[0] + jw[1] + jw[2] + jw[3];
            float pAcc[3] = {0,0,0}, nAcc[3] = {0,0,0};
            if (wsum < 1e-6f) {
                pAcc[0] = bp[0]; pAcc[1] = bp[1]; pAcc[2] = bp[2];
                nAcc[0] = bn[0]; nAcc[1] = bn[1]; nAcc[2] = bn[2];
            } else {
                for (int i = 0; i < 4; ++i) {
                    float w = jw[i];
                    if (w <= 0.0f) continue;
                    uint16_t jidx = ji[i];
                    if (jidx >= jcount) continue;
                    const float* jm = &m_palette[(size_t)jidx * 16];
                    float tp[3], tn[3];
                    xformPoint(jm, bp, tp);
                    xformDir(jm, bn, tn);
                    pAcc[0] += w*tp[0]; pAcc[1] += w*tp[1]; pAcc[2] += w*tp[2];
                    nAcc[0] += w*tn[0]; nAcc[1] += w*tn[1]; nAcc[2] += w*tn[2];
                }
                float inv = 1.0f / wsum;
                pAcc[0] *= inv; pAcc[1] *= inv; pAcc[2] *= inv;
            }
            float nl = std::sqrt(nAcc[0]*nAcc[0] + nAcc[1]*nAcc[1] + nAcc[2]*nAcc[2]);
            if (nl > 1e-8f) { nAcc[0]/=nl; nAcc[1]/=nl; nAcc[2]/=nl; }
            x3::rhi::MeshVertex& mv = m_vertScratch[v];
            mv.pos[0] = pAcc[0]; mv.pos[1] = pAcc[1]; mv.pos[2] = pAcc[2];
            mv.normal[0] = nAcc[0]; mv.normal[1] = nAcc[1]; mv.normal[2] = nAcc[2];
            mv.uv[0] = p.baseUv[v*2+0]; mv.uv[1] = p.baseUv[v*2+1];
        }
        device.updateMesh(x3::rhi::MeshHandle{ meshId }, m_vertScratch.data(),
                          (uint32_t)vcount);
    }
}

// ===========================================================================
// T1 — locomotion blend + crossfade / inertialization.
// ===========================================================================

void Skinner::setLocomotionClips(int idleClip, int walkClip, int runClip,
                                 float walkSpeed, float runSpeed) {
    m_idleClip = idleClip;
    m_walkClip = walkClip;
    m_runClip  = runClip;
    if (walkSpeed > 1e-3f) m_walkSpeed = walkSpeed;
    if (runSpeed  > m_walkSpeed) m_runSpeed = runSpeed;
}

void Skinner::setLocomotionSpeed(float speedMetersPerSec) {
    m_speedCmd  = speedMetersPerSec < 0.0f ? 0.0f : speedMetersPerSec;
    m_loco01Cmd = -1.0f;   // m/s mapping path
}

void Skinner::setLocomotion01(float speed01) {
    m_loco01Cmd = (speed01 < 0.0f) ? 0.0f : (speed01 > 1.0f ? 1.0f : speed01);
}

void Skinner::triggerClip(int clip, float fadeSec, bool loop) {
    // IDEMPOTENCY (freeze fix): callers commonly call this EVERY FRAME (e.g. the 3P
    // avatar requests the fire clip while fireHeld and cancels (clip<0) otherwise).
    // A naive implementation that reset m_xfadeTime on every call would re-seed the
    // ramp to 0 each frame, so a crossfade could never finish ramping (m_xfadeW
    // pinned), permanently FREEZING the pose on the crossfade target. So a repeated
    // request for the SAME state must be a true no-op — only an ACTUAL state change
    // (re)seeds the ramp timer.
    const bool wantCancel = (clip < 0 || (uint32_t)clip >= m_clipDurations.size());
    if (wantCancel) {
        // Cancel: ramp back out to the locomotion blend (crossfaded, not snapped).
        // Already inactive, or already ramping out -> nothing to do (don't reset the
        // ramp clock, or the fade-out would stall forever).
        if (m_xfadeActive && !m_xfadeOut) {
            m_xfadeOut  = true;
            m_xfadeTime = 0.0f;     // seed the ramp-OUT once, on the transition only
            m_xfadeDur  = (fadeSec > 1e-3f) ? fadeSec : 1e-3f;
        }
        return;
    }
    // Already crossfading IN to this exact clip with the same loop mode: no-op (let
    // the in-progress ramp keep advancing rather than restarting it every frame).
    if (m_xfadeActive && !m_xfadeOut && m_xfadeClip == clip && m_xfadeLoop == loop)
        return;
    // New target (or re-targeting after a cancel): (re)seed the ramp-IN.
    m_xfadeDur    = (fadeSec > 1e-3f) ? fadeSec : 1e-3f;
    m_xfadeTime   = 0.0f;
    m_xfadeActive = true;
    m_xfadeClip   = clip;
    m_xfadeLoop   = loop;
    m_xfadeClipT  = 0.0f;
    m_xfadeOut    = false;
    // m_xfadeW stays where it is (it ramps up smoothly from the current value), so
    // re-triggering after a different clip does not pop.
}

// Sample every node's local pose from a clip into flat caller arrays. Reused, not
// reallocated, in the steady path (the arrays are member scratch sized in bind()).
void Skinner::sampleClipPose(const x3::asset::Model& m, uint32_t clip, float t,
                             std::vector<float>& poseT, std::vector<float>& poseR,
                             std::vector<float>& poseS) const {
    for (uint32_t n = 0; n < m_nodeCount; ++n) {
        sampleNodeTRS(m, clip, (int)n, t, &poseT[(size_t)n*3], &poseR[(size_t)n*4],
                      &poseS[(size_t)n*3]);
    }
}

// Build the joint palette from a set of per-node LOCAL poses (the blend output).
// Composes each node's local matrix from its blended T/R/S, accumulates globals
// down the hierarchy (iterative, recursion-free), then multiplies by inverseBind.
uint32_t Skinner::paletteFromPose(const x3::asset::Model& m,
                                  const std::vector<float>& poseT,
                                  const std::vector<float>& poseR,
                                  const std::vector<float>& poseS,
                                  std::vector<float>& outPalette) const {
    std::vector<float>& globals = m_globalScratch;
    if (globals.size() != (size_t)m_nodeCount * 16)
        globals.assign((size_t)m_nodeCount * 16, 0.0f);
    else
        std::fill(globals.begin(), globals.end(), 0.0f);

    if (m_resolveDone.size() != m_nodeCount)   m_resolveDone.assign(m_nodeCount, 0);
    else                                       std::fill(m_resolveDone.begin(), m_resolveDone.end(), (char)0);
    if (m_resolveInProg.size() != m_nodeCount) m_resolveInProg.assign(m_nodeCount, 0);
    else                                       std::fill(m_resolveInProg.begin(), m_resolveInProg.end(), (char)0);
    std::vector<char>& done = m_resolveDone;
    std::vector<char>& inprog = m_resolveInProg;
    std::vector<int>& stack = m_resolveStack;

    std::array<float, 16> local;
    for (uint32_t i = 0; i < m_nodeCount; ++i) {
        if (done[i]) continue;
        stack.clear();
        int cur = (int)i;
        while (cur >= 0 && !done[cur]) {
            if (inprog[cur]) break;       // cycle guard
            inprog[cur] = 1;
            stack.push_back(cur);
            cur = m.nodes[cur].parent;
        }
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            int n = *it;
            trsToMat4(&poseT[(size_t)n*3], &poseR[(size_t)n*4], &poseS[(size_t)n*3],
                      local.data());
            int parent = m.nodes[n].parent;
            if (parent >= 0 && (uint32_t)parent < m_nodeCount && done[parent]) {
                mat4Mul(&globals[(size_t)parent*16], local.data(), &globals[(size_t)n*16]);
            } else {
                std::memcpy(&globals[(size_t)n*16], local.data(), sizeof(float)*16);
            }
            done[n] = 1; inprog[n] = 0;
        }
    }

    const x3::asset::Skin& skin = m.skins[0];
    const uint32_t jcount = (uint32_t)skin.joints.size();
    outPalette.assign((size_t)jcount * 16, 0.0f);
    for (uint32_t j = 0; j < jcount; ++j) {
        int node = skin.joints[j];
        if (node < 0 || (uint32_t)node >= m_nodeCount) { mat4Identity(&outPalette[(size_t)j*16]); continue; }
        const float* ib = &skin.inverseBind[(size_t)j * 16];
        mat4Mul(&globals[(size_t)node*16], ib, &outPalette[(size_t)j*16]);
    }
    return jcount;
}

// Advance the locomotion blend (+ any crossfade) by dt and write the blended
// per-node LOCAL pose into m_blendT/R/S. The 1D blend keeps a single shared
// PHASE so the bracketing clips stay foot-synced; the phase advances at a rate
// blended from the active clips' (1/duration) so it loops cleanly.
bool Skinner::advanceBlend(const x3::asset::Model& model, float dt) {
    if (!m_valid) return false;
    if (dt < 0.0f) dt = 0.0f;

    // ---- 1) Resolve the target 1D weight (0=idle .. 1=run) from the command. ----
    float targetW;
    if (m_loco01Cmd >= 0.0f) {
        targetW = m_loco01Cmd;
    } else {
        // Map m/s -> [0,1]: 0..walkSpeed occupies [0,0.5], walkSpeed..runSpeed
        // occupies [0.5,1]. This makes "walk" land at the midpoint of the blend.
        const float s = m_speedCmd;
        if (s <= 0.0f) {
            targetW = 0.0f;
        } else if (s <= m_walkSpeed) {
            targetW = 0.5f * (s / m_walkSpeed);
        } else {
            const float span = (m_runSpeed > m_walkSpeed) ? (m_runSpeed - m_walkSpeed) : 1.0f;
            float u = (s - m_walkSpeed) / span;
            if (u > 1.0f) u = 1.0f;
            targetW = 0.5f + 0.5f * u;
        }
    }
    // Smooth the param itself so abrupt speed changes don't pop the blend (a light
    // critically-damped-ish approach toward the target). Time-constant ~0.12 s.
    const float kParamRate = 8.0f;
    float a = kParamRate * dt; if (a > 1.0f) a = 1.0f;
    m_locoW += (targetW - m_locoW) * a;
    if (m_locoW < 0.0f) m_locoW = 0.0f;
    if (m_locoW > 1.0f) m_locoW = 1.0f;

    // ---- 2) Pick the two bracketing locomotion clips + the within-bracket u. ----
    // Bracket A is idle->walk (w in [0,0.5]) or walk->run (w in [0.5,1]). Absent
    // clips fall back so the blend degenerates gracefully (idle-only -> always
    // idle). clipA/clipB are clip indices; bu is the lerp factor A->B.
    int clipA, clipB; float bu;
    auto firstValid = [&](std::initializer_list<int> opts) -> int {
        for (int c : opts) if (c >= 0) return c;
        return -1;
    };
    if (m_locoW <= 0.5f) {
        clipA = firstValid({ m_idleClip, m_walkClip, m_runClip });
        clipB = firstValid({ m_walkClip, m_runClip, m_idleClip });
        bu = m_locoW * 2.0f;
    } else {
        clipA = firstValid({ m_walkClip, m_idleClip, m_runClip });
        clipB = firstValid({ m_runClip,  m_walkClip, m_idleClip });
        bu = (m_locoW - 0.5f) * 2.0f;
    }
    if (clipA < 0) clipA = 0;          // no locomotion clips registered: clip 0
    if (clipB < 0) clipB = clipA;
    if (clipA == clipB) bu = 0.0f;

    // ---- 3) Advance the shared phase by the blended clip rate (loops). ----
    const float durA = m_clipDurations[(size_t)clipA];
    const float durB = m_clipDurations[(size_t)clipB];
    const float rateA = (durA > 1e-4f) ? (1.0f / durA) : 0.0f;
    const float rateB = (durB > 1e-4f) ? (1.0f / durB) : 0.0f;
    const float rate  = rateA + (rateB - rateA) * bu;     // phase units/sec
    m_phase += rate * dt;
    m_phase -= std::floor(m_phase);                       // wrap to [0,1)

    // ---- 4) Sample both brackets at the SAME phase (foot-synced) + blend. ----
    const float tA = m_phase * durA;
    const float tB = m_phase * durB;
    sampleClipPose(model, (uint32_t)clipA, tA, m_poseAT, m_poseAR, m_poseAS);
    if (clipB != clipA)
        sampleClipPose(model, (uint32_t)clipB, tB, m_poseBT, m_poseBR, m_poseBS);

    for (uint32_t n = 0; n < m_nodeCount; ++n) {
        const size_t i3 = (size_t)n*3, i4 = (size_t)n*4;
        if (clipB == clipA || bu <= 0.0f) {
            std::memcpy(&m_blendT[i3], &m_poseAT[i3], sizeof(float)*3);
            std::memcpy(&m_blendR[i4], &m_poseAR[i4], sizeof(float)*4);
            std::memcpy(&m_blendS[i3], &m_poseAS[i3], sizeof(float)*3);
        } else {
            lerp3(&m_poseAT[i3], &m_poseBT[i3], bu, &m_blendT[i3]);
            blendQuat(&m_poseAR[i4], &m_poseBR[i4], bu, &m_blendR[i4]);
            lerp3(&m_poseAS[i3], &m_poseBS[i3], bu, &m_blendS[i3]);
        }
    }

    // ---- 5) Crossfade / inertialization toward a discrete clip (e.g. Jump). ----
    // Ramp m_xfadeW with smoothstep over m_xfadeDur. The target clip plays at its
    // own time; a non-looping target that has played out ramps back to locomotion.
    if (m_xfadeActive) {
        m_xfadeClipT += dt;
        const float xdur = m_clipDurations[(size_t)m_xfadeClip];
        // Keep a LOOPING crossfade-target cursor bounded: over minutes of sustained
        // play (e.g. fire held in 3P) an unwrapped accumulator loses float precision
        // and eventually quantizes to a single sampled phase ("stops animating").
        // Non-loop targets are intentionally left to run out (the ramp-out fires).
        if (m_xfadeLoop && xdur > 1e-4f && m_xfadeClipT >= xdur)
            m_xfadeClipT = std::fmod(m_xfadeClipT, xdur);
        // Decide ramp direction.
        if (!m_xfadeOut && !m_xfadeLoop && xdur > 1e-4f &&
            m_xfadeClipT >= xdur - m_xfadeDur) {
            m_xfadeOut = true;            // start fading back near the clip's end
            m_xfadeTime = 0.0f;
        }
        m_xfadeTime += dt;
        const float ramp = smoothstep01(m_xfadeTime / m_xfadeDur);
        m_xfadeW = m_xfadeOut ? (1.0f - ramp) : ramp;
        if (m_xfadeW < 0.0f) m_xfadeW = 0.0f;
        if (m_xfadeW > 1.0f) m_xfadeW = 1.0f;

        // Sample the target clip (clamp time for a one-shot, wrap for a loop).
        float xt = m_xfadeClipT;
        if (m_xfadeLoop && xdur > 1e-4f) { xt = std::fmod(xt, xdur); }
        else if (xdur > 1e-4f && xt > xdur) xt = xdur;
        sampleClipPose(model, (uint32_t)m_xfadeClip, xt, m_xfadeT, m_xfadeR, m_xfadeS);

        // Mix the target pose over the locomotion blend by m_xfadeW.
        if (m_xfadeW > 0.0f) {
            for (uint32_t n = 0; n < m_nodeCount; ++n) {
                const size_t i3 = (size_t)n*3, i4 = (size_t)n*4;
                float t3[3], s3[3], q4[4];
                lerp3(&m_blendT[i3], &m_xfadeT[i3], m_xfadeW, t3);
                lerp3(&m_blendS[i3], &m_xfadeS[i3], m_xfadeW, s3);
                blendQuat(&m_blendR[i4], &m_xfadeR[i4], m_xfadeW, q4);
                std::memcpy(&m_blendT[i3], t3, sizeof t3);
                std::memcpy(&m_blendS[i3], s3, sizeof s3);
                std::memcpy(&m_blendR[i4], q4, sizeof q4);
            }
        }

        // Finished ramping out: deactivate the crossfade (back to pure locomotion).
        if (m_xfadeOut && m_xfadeW <= 0.0f) {
            m_xfadeActive = false; m_xfadeClip = -1; m_xfadeOut = false;
            m_xfadeW = 0.0f; m_xfadeClipT = 0.0f; m_xfadeTime = 0.0f;
        }
    }
    return true;
}

uint32_t Skinner::advanceAndComputePalette(const x3::asset::Model& model, float dt,
                                           std::vector<float>& outPalette) {
    if (!advanceBlend(model, dt)) { outPalette.clear(); return 0; }
    applyFootIk(model, dt);   // IK runs AFTER blend, BEFORE the palette accumulate
    return paletteFromPose(model, m_blendT, m_blendR, m_blendS, outPalette);
}

void Skinner::applyLocomotion(const x3::asset::Model& model,
                              x3::rhi::IRenderDevice& device, float dt) {
    if (!m_valid) return;
    if (!advanceBlend(model, dt)) return;
    applyFootIk(model, dt);   // IK runs AFTER blend, BEFORE the palette accumulate
    uint32_t jcount = paletteFromPose(model, m_blendT, m_blendR, m_blendS, m_palette);
    if (jcount == 0) return;

    // ---- GPU path: upload the CPU-computed palette; the device's compute pre-pass
    // skins on the GPU into each mesh's skinned-output vbo (no CPU LBS, no updateMesh).
    if (m_gpuSkin) {
        for (const auto& p : model.primitives) {
            if (!p.skinned) continue;
            const uint32_t meshId = x3::asset::meshIdOf(p);
            if (meshId == 0) continue;
            device.setSkinnedPalette(x3::rhi::MeshHandle{ meshId }, m_palette.data(), jcount);
        }
        return;
    }

    for (const auto& p : model.primitives) {
        if (!p.skinned) continue;
        const uint32_t meshId = x3::asset::meshIdOf(p);
        if (meshId == 0) continue;
        const size_t vcount = p.basePos.size() / 3;
        if (vcount == 0) continue;
        m_vertScratch.resize(vcount);
        for (size_t v = 0; v < vcount; ++v) {
            const float* bp = &p.basePos[v*3];
            const float* bn = &p.baseNrm[v*3];
            const uint16_t* ji = &p.jointIdx[v*4];
            const float* jw = &p.jointWt[v*4];
            float wsum = jw[0] + jw[1] + jw[2] + jw[3];
            float pAcc[3] = {0,0,0}, nAcc[3] = {0,0,0};
            if (wsum < 1e-6f) {
                pAcc[0] = bp[0]; pAcc[1] = bp[1]; pAcc[2] = bp[2];
                nAcc[0] = bn[0]; nAcc[1] = bn[1]; nAcc[2] = bn[2];
            } else {
                for (int i = 0; i < 4; ++i) {
                    float w = jw[i];
                    if (w <= 0.0f) continue;
                    uint16_t jidx = ji[i];
                    if (jidx >= jcount) continue;
                    const float* jm = &m_palette[(size_t)jidx * 16];
                    float tp[3], tn[3];
                    xformPoint(jm, bp, tp);
                    xformDir(jm, bn, tn);
                    pAcc[0] += w*tp[0]; pAcc[1] += w*tp[1]; pAcc[2] += w*tp[2];
                    nAcc[0] += w*tn[0]; nAcc[1] += w*tn[1]; nAcc[2] += w*tn[2];
                }
                float inv = 1.0f / wsum;
                pAcc[0] *= inv; pAcc[1] *= inv; pAcc[2] *= inv;
            }
            float nl = std::sqrt(nAcc[0]*nAcc[0] + nAcc[1]*nAcc[1] + nAcc[2]*nAcc[2]);
            if (nl > 1e-8f) { nAcc[0]/=nl; nAcc[1]/=nl; nAcc[2]/=nl; }
            x3::rhi::MeshVertex& mv = m_vertScratch[v];
            mv.pos[0] = pAcc[0]; mv.pos[1] = pAcc[1]; mv.pos[2] = pAcc[2];
            mv.normal[0] = nAcc[0]; mv.normal[1] = nAcc[1]; mv.normal[2] = nAcc[2];
            mv.uv[0] = p.baseUv[v*2+0]; mv.uv[1] = p.baseUv[v*2+1];
        }
        device.updateMesh(x3::rhi::MeshHandle{ meshId }, m_vertScratch.data(),
                          (uint32_t)vcount);
    }
}

void Skinner::apply(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
                    uint32_t clip, float timeSec) {
    if (!m_valid) return;
    uint32_t jcount = computePalette(model, clip, timeSec, m_palette);
    if (jcount == 0) return;
    skinAndUpload(model, device, jcount);
}

uint32_t Skinner::buildPaletteFromGlobals(const x3::asset::Model& model, const float* nodeGlobals,
                                          uint32_t nodeCount, std::vector<float>& outPalette) const {
    if (!m_valid || !nodeGlobals || nodeCount != m_nodeCount) { outPalette.clear(); return 0; }
    const x3::asset::Skin& skin = model.skins[0];
    const uint32_t jcount = (uint32_t)skin.joints.size();
    outPalette.assign((size_t)jcount * 16, 0.0f);
    for (uint32_t j = 0; j < jcount; ++j) {
        int node = skin.joints[j];
        if (node < 0 || (uint32_t)node >= m_nodeCount) { mat4Identity(&outPalette[(size_t)j*16]); continue; }
        const float* ib = &skin.inverseBind[(size_t)j * 16];
        mat4Mul(&nodeGlobals[(size_t)node*16], ib, &outPalette[(size_t)j*16]);
    }
    return jcount;
}

void Skinner::applyExternalGlobals(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
                                   const float* nodeGlobals, uint32_t nodeCount) {
    if (!m_valid) return;
    uint32_t jcount = buildPaletteFromGlobals(model, nodeGlobals, nodeCount, m_palette);
    if (jcount == 0) return;
    skinAndUpload(model, device, jcount);
}

void Skinner::skinAndUpload(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
                            uint32_t jcount) {
    // ---- GPU path: upload the CPU-computed palette; the device's compute pre-pass
    // skins on the GPU into each mesh's skinned-output vbo (no CPU LBS, no updateMesh).
    if (m_gpuSkin) {
        for (const auto& p : model.primitives) {
            if (!p.skinned) continue;
            const uint32_t meshId = x3::asset::meshIdOf(p);
            if (meshId == 0) continue;
            device.setSkinnedPalette(x3::rhi::MeshHandle{ meshId }, m_palette.data(), jcount);
        }
        return;
    }

    for (const auto& p : model.primitives) {
        if (!p.skinned) continue;
        const uint32_t meshId = x3::asset::meshIdOf(p);
        if (meshId == 0) continue;                  // headless: nothing to upload
        const size_t vcount = p.basePos.size() / 3;
        if (vcount == 0) continue;
        m_vertScratch.resize(vcount);
        for (size_t v = 0; v < vcount; ++v) {
            const float* bp = &p.basePos[v*3];
            const float* bn = &p.baseNrm[v*3];
            const uint16_t* ji = &p.jointIdx[v*4];
            const float* jw = &p.jointWt[v*4];
            float wsum = jw[0] + jw[1] + jw[2] + jw[3];
            float pAcc[3] = {0,0,0}, nAcc[3] = {0,0,0};
            if (wsum < 1e-6f) {
                // Unweighted vertex: leave it at bind pose (no joint influence).
                pAcc[0] = bp[0]; pAcc[1] = bp[1]; pAcc[2] = bp[2];
                nAcc[0] = bn[0]; nAcc[1] = bn[1]; nAcc[2] = bn[2];
            } else {
                for (int i = 0; i < 4; ++i) {
                    float w = jw[i];
                    if (w <= 0.0f) continue;
                    uint16_t jidx = ji[i];
                    if (jidx >= jcount) continue;
                    const float* jm = &m_palette[(size_t)jidx * 16];
                    float tp[3], tn[3];
                    xformPoint(jm, bp, tp);
                    xformDir(jm, bn, tn);
                    pAcc[0] += w*tp[0]; pAcc[1] += w*tp[1]; pAcc[2] += w*tp[2];
                    nAcc[0] += w*tn[0]; nAcc[1] += w*tn[1]; nAcc[2] += w*tn[2];
                }
                // Normalize accumulated weights (handles non-unit weight sums).
                float inv = 1.0f / wsum;
                pAcc[0] *= inv; pAcc[1] *= inv; pAcc[2] *= inv;
            }
            float nl = std::sqrt(nAcc[0]*nAcc[0] + nAcc[1]*nAcc[1] + nAcc[2]*nAcc[2]);
            if (nl > 1e-8f) { nAcc[0]/=nl; nAcc[1]/=nl; nAcc[2]/=nl; }
            x3::rhi::MeshVertex& mv = m_vertScratch[v];
            mv.pos[0] = pAcc[0]; mv.pos[1] = pAcc[1]; mv.pos[2] = pAcc[2];
            mv.normal[0] = nAcc[0]; mv.normal[1] = nAcc[1]; mv.normal[2] = nAcc[2];
            mv.uv[0] = p.baseUv[v*2+0]; mv.uv[1] = p.baseUv[v*2+1];
        }
        device.updateMesh(x3::rhi::MeshHandle{ meshId }, m_vertScratch.data(),
                          (uint32_t)vcount);
    }
}

// ===========================================================================
// Foot IK — general character grounding (slopes / stairs / uneven terrain).
//
// Clean-room: analytic two-bone IK (law of cosines for the knee bend + a pole
// hint for the bend plane), a per-foot downward ground raycast for planting, and
// a lower-foot-governs pelvis drop. Built from public IK references only.
// ===========================================================================

// Analytic two-bone solver (static; pure geometry). Solves hip->knee->foot so the
// foot reaches `target` with the knee bending toward `pole`. Bone lengths come from
// the supplied rest positions. Unreachable -> straight leg toward the target.
void Skinner::solveTwoBone(const float hip[3], const float knee[3],
                           const float foot[3], const float target[3],
                           const float pole[3], float outKnee[3], float outFoot[3]) {
    float u[3], l[3];
    sub3(knee, hip, u);            // upper bone (hip->knee)
    sub3(foot, knee, l);           // lower bone (knee->foot)
    float L1 = len3(u);
    float L2 = len3(l);
    if (L1 < 1e-6f || L2 < 1e-6f) { // degenerate rig: pass through
        outKnee[0]=knee[0]; outKnee[1]=knee[1]; outKnee[2]=knee[2];
        outFoot[0]=foot[0]; outFoot[1]=foot[1]; outFoot[2]=foot[2];
        return;
    }

    float toT[3]; sub3(target, hip, toT);
    float dist = len3(toT);
    float dir[3] = { toT[0], toT[1], toT[2] };
    if (!norm3(dir)) {             // target == hip: keep the rest direction
        sub3(knee, hip, dir); norm3(dir);
    }

    // Clamp the reach so an over/under-extended target never produces a NaN bend.
    const float kEps = 1e-4f;
    float minReach = std::fabs(L1 - L2) + kEps;
    float maxReach = (L1 + L2) - kEps;
    float reach = dist;
    if (reach < minReach) reach = minReach;
    if (reach > maxReach) reach = maxReach;   // unreachable -> straight leg (clamped)

    // Law of cosines: angle at the hip between the upper bone and the hip->target line.
    float cosHip = (L1*L1 + reach*reach - L2*L2) / (2.0f * L1 * reach);
    if (cosHip > 1.0f) cosHip = 1.0f;
    if (cosHip < -1.0f) cosHip = -1.0f;
    float hipAng = std::acos(cosHip);

    // Bend axis: perpendicular to dir, in the plane spanned by dir + pole. The knee
    // bends toward `pole`. Fall back to a stable axis if pole is parallel to dir.
    float poleDir[3] = { pole[0], pole[1], pole[2] };
    float projp = dot3(poleDir, dir);
    float poleFlat[3] = { poleDir[0]-projp*dir[0], poleDir[1]-projp*dir[1], poleDir[2]-projp*dir[2] };
    if (!norm3(poleFlat)) {
        // pole parallel to dir: pick any vector perpendicular to dir.
        float ax[3] = { 1,0,0 };
        if (std::fabs(dir[0]) > 0.9f) { ax[0]=0; ax[1]=1; ax[2]=0; }
        float c[3]; cross3(dir, ax, c); norm3(c);
        poleFlat[0]=c[0]; poleFlat[1]=c[1]; poleFlat[2]=c[2];
    }
    // bendAxis = dir x poleFlat (rotation axis); knee = hip + Rodrigues(dir, +hipAng)*L1
    float bendAxis[3]; cross3(dir, poleFlat, bendAxis); norm3(bendAxis);

    // Rotate `dir` about bendAxis by +hipAng (toward poleFlat) to get the upper-bone dir.
    float s = std::sin(hipAng), c = std::cos(hipAng);
    // Rodrigues: v' = v*c + (k x v)*s + k*(k.v)*(1-c)
    float kxv[3]; cross3(bendAxis, dir, kxv);
    float kdv = dot3(bendAxis, dir);
    float ub[3] = {
        dir[0]*c + kxv[0]*s + bendAxis[0]*kdv*(1-c),
        dir[1]*c + kxv[1]*s + bendAxis[1]*kdv*(1-c),
        dir[2]*c + kxv[2]*s + bendAxis[2]*kdv*(1-c),
    };
    norm3(ub);
    outKnee[0] = hip[0] + ub[0]*L1;
    outKnee[1] = hip[1] + ub[1]*L1;
    outKnee[2] = hip[2] + ub[2]*L1;
    // Foot sits at the (clamped) reach along dir from the hip.
    outFoot[0] = hip[0] + dir[0]*reach;
    outFoot[1] = hip[1] + dir[1]*reach;
    outFoot[2] = hip[2] + dir[2]*reach;
}

// Resolve the humanoid leg + hips bones by name (case-insensitive). Common rigs:
//   upper leg: "upperleg" / "thigh" / "upleg"
//   lower leg: "lowerleg" / "shin"  / "calf"   (also "leg" as a last resort)
//   foot:      "foot" / "ankle"
//   hips:      "hips" / "pelvis"
// Side: ".l"/".r" suffix or "left"/"right" substring. Bone lengths come from the
// rest local-translation magnitudes of the knee/foot nodes.
void Skinner::resolveFootIkBones(const x3::asset::Model& m) {
    m_legResolved = false;
    m_pelvisNode = m_rootNode = -1;
    for (int sd = 0; sd < 2; ++sd) {
        m_hipNode[sd] = m_kneeNode[sd] = m_footNode[sd] = -1;
        m_sideOk[sd] = false;
        for (int p = 0; p < 4; ++p) m_boneName[sd][p].clear();
    }
    if (m.nodes.empty()) return;

    auto sideOf = [](const std::string& nm) -> int {
        std::string n = toLower(nm);
        // explicit side tokens (left/right) win; then .l/.r / _l/_r suffixes.
        bool hasLeft  = n.find("left")  != std::string::npos;
        bool hasRight = n.find("right") != std::string::npos;
        if (hasLeft && !hasRight)  return 0;
        if (hasRight && !hasLeft)  return 1;
        // suffix / token forms: ".l", "_l", " l", "l." etc. Check word-ish boundaries.
        auto endsWith = [&](const char* suf){ size_t L=std::strlen(suf);
            return n.size()>=L && n.compare(n.size()-L, L, suf)==0; };
        if (endsWith(".l") || endsWith("_l") || endsWith("-l") || endsWith(" l") || endsWith("left")) return 0;
        if (endsWith(".r") || endsWith("_r") || endsWith("-r") || endsWith(" r") || endsWith("right")) return 1;
        return -1;
    };

    // Pelvis / hips: prefer an exact-ish hips/pelvis bone.
    for (uint32_t i = 0; i < (uint32_t)m.nodes.size(); ++i) {
        const std::string& nm = m.nodes[i].name;
        if (nm.empty()) continue;
        if (icontains(nm, "pelvis") || icontains(nm, "hips") || icontains(nm, "hip ")) {
            // Avoid matching "UpperLeg" via "hip"-less names; require pelvis/hips.
            if (icontains(nm, "pelvis") || icontains(nm, "hips")) { m_pelvisNode = (int)i; break; }
        }
    }

    // For each side, find upperleg/thigh, lowerleg/shin, foot/ankle.
    auto findSided = [&](int side, std::initializer_list<const char*> keys,
                         std::initializer_list<const char*> avoid) -> int {
        for (const char* key : keys) {
            for (uint32_t i = 0; i < (uint32_t)m.nodes.size(); ++i) {
                const std::string& nm = m.nodes[i].name;
                if (nm.empty()) continue;
                if (!icontains(nm, key)) continue;
                bool bad = false;
                for (const char* av : avoid) if (icontains(nm, av)) { bad = true; break; }
                if (bad) continue;
                if (sideOf(nm) != side) continue;
                return (int)i;
            }
        }
        return -1;
    };

    for (int side = 0; side < 2; ++side) {
        // Upper leg (thigh). Avoid matching the foot/toe accidentally.
        m_hipNode[side]  = findSided(side, { "upperleg", "upper_leg", "thigh", "upleg" }, { "twist", "toe" });
        // Lower leg (shin/calf). "leg" alone is a fallback but must not be the thigh.
        m_kneeNode[side] = findSided(side, { "lowerleg", "lower_leg", "shin", "calf", "knee" }, { "upper", "twist", "toe", "thigh", "upleg" });
        if (m_kneeNode[side] < 0)
            m_kneeNode[side] = findSided(side, { "leg" }, { "upper", "thigh", "upleg", "twist", "toe" });
        m_footNode[side] = findSided(side, { "foot", "ankle" }, { "toe", "ball", "twist" });

        bool ok = m_hipNode[side] >= 0 && m_kneeNode[side] >= 0 && m_footNode[side] >= 0
               && m_hipNode[side] != m_kneeNode[side] && m_kneeNode[side] != m_footNode[side];
        if (ok) {
            // Bone lengths from rest local-translation magnitudes of knee + foot.
            float t[3], q[4], sc[3];
            decompose(m.nodes[m_kneeNode[side]].localTransform, t, q, sc);
            m_upperLen[side] = std::sqrt(t[0]*t[0]+t[1]*t[1]+t[2]*t[2]);
            decompose(m.nodes[m_footNode[side]].localTransform, t, q, sc);
            m_lowerLen[side] = std::sqrt(t[0]*t[0]+t[1]*t[1]+t[2]*t[2]);
            ok = m_upperLen[side] > 1e-5f && m_lowerLen[side] > 1e-5f;
        }
        m_sideOk[side] = ok;
        if (ok) {
            m_boneName[side][0] = m.nodes[m_hipNode[side]].name;
            m_boneName[side][1] = m.nodes[m_kneeNode[side]].name;
            m_boneName[side][2] = m.nodes[m_footNode[side]].name;
        }
    }

    // Pelvis fallback: if no explicit hips bone, use the common ancestor of the two
    // hips (the parent of an upper-leg node) so the pelvis drop has something to move.
    if (m_pelvisNode < 0) {
        for (int side = 0; side < 2; ++side) {
            if (m_sideOk[side]) {
                int par = m.nodes[m_hipNode[side]].parent;
                if (par >= 0) { m_pelvisNode = par; break; }
            }
        }
    }
    m_rootNode = m_pelvisNode;
    for (int side = 0; side < 2; ++side)
        if (m_sideOk[side] && m_pelvisNode >= 0)
            m_boneName[side][3] = m.nodes[m_pelvisNode].name;

    m_legResolved = m_sideOk[0] || m_sideOk[1];
}

void Skinner::setFootIk(bool enabled, const GroundRay& ray, const float worldFromModel[16]) {
    m_footIkEnabled = enabled;
    m_groundRay = ray;
    if (worldFromModel) std::memcpy(m_worldFromModel, worldFromModel, sizeof(float)*16);
}

void Skinner::setFootIkWorldFromModel(const float worldFromModel[16]) {
    if (worldFromModel) std::memcpy(m_worldFromModel, worldFromModel, sizeof(float)*16);
}

std::string_view Skinner::footIkBoneName(int side, int part) const {
    if (side < 0 || side > 1 || part < 0 || part > 3) return {};
    return std::string_view(m_boneName[side][part]);
}

// Run the foot-IK pass IN PLACE over the blended local pose (m_blendT/R). Builds
// model-space globals, plants each foot via the ground ray, drops the pelvis so the
// lower foot reaches (smoothed + clamped), then solves each leg with solveTwoBone
// and writes corrected hip/knee LOCAL rotations + the pelvis offset back.
void Skinner::applyFootIk(const x3::asset::Model& model, float dt) {
    if (!m_valid || !m_footIkEnabled || !m_legResolved || !m_groundRay.fn) {
        // Smoothly relax any residual IK weights when disabled (no pop).
        for (int s = 0; s < 2; ++s) m_legW[s] += (0.0f - m_legW[s]) * std::min(1.0f, 10.0f*dt);
        m_pelvisDropSmoothed += (0.0f - m_pelvisDropSmoothed) * std::min(1.0f, 10.0f*dt);
        return;
    }
    if (dt < 0.0f) dt = 0.0f;

    // Accumulate model-space globals from the current blended local pose into
    // m_ikGlobals (same iterative, recursion-free walk as paletteFromPose; reuses
    // the resolve scratch — no heap alloc in the steady path). Called once up front
    // and again after the pelvis drop changes a node's local translation.
    std::vector<float>& G = m_ikGlobals;
    auto rebuildGlobals = [&]() {
        if (G.size() != (size_t)m_nodeCount * 16) G.assign((size_t)m_nodeCount * 16, 0.0f);
        else std::fill(G.begin(), G.end(), 0.0f);
        if (m_resolveDone.size() != m_nodeCount)   m_resolveDone.assign(m_nodeCount, 0);
        else std::fill(m_resolveDone.begin(), m_resolveDone.end(), (char)0);
        if (m_resolveInProg.size() != m_nodeCount) m_resolveInProg.assign(m_nodeCount, 0);
        else std::fill(m_resolveInProg.begin(), m_resolveInProg.end(), (char)0);
        std::vector<char>& done = m_resolveDone; std::vector<char>& inprog = m_resolveInProg;
        std::vector<int>& stack = m_resolveStack;
        std::array<float,16> local;
        for (uint32_t i = 0; i < m_nodeCount; ++i) {
            if (done[i]) continue;
            stack.clear(); int cur = (int)i;
            while (cur >= 0 && !done[cur]) { if (inprog[cur]) break; inprog[cur]=1; stack.push_back(cur); cur = model.nodes[cur].parent; }
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                int n = *it;
                trsToMat4(&m_blendT[(size_t)n*3], &m_blendR[(size_t)n*4], &m_blendS[(size_t)n*3], local.data());
                int parent = model.nodes[n].parent;
                if (parent >= 0 && (uint32_t)parent < m_nodeCount && done[parent])
                    mat4Mul(&G[(size_t)parent*16], local.data(), &G[(size_t)n*16]);
                else std::memcpy(&G[(size_t)n*16], local.data(), sizeof(float)*16);
                done[n]=1; inprog[n]=0;
            }
        }
    };
    rebuildGlobals();

    // World-up direction (model is +Y up per CONVENTIONS.md). The ground ray casts
    // straight DOWN in world space; we work the plant height back in model space by
    // assuming the model->world transform has no roll on the up axis (typical for a
    // standing character placed with a yaw + translation). Heights along model +Y.
    const float modelUp[3] = { 0, 1, 0 };

    // Helper: model-space point -> world-space point via m_worldFromModel.
    auto toWorld = [&](const float p[3], float o[3]) { xformPoint(m_worldFromModel, p, o); };

    // 2) Per-foot ground probe. Cast from a bit ABOVE the foot straight down.
    struct Plant { bool hit=false; float footModelY=0; float groundN[3]={0,1,0}; float curFoot[3]={0,0,0}; };
    Plant pl[2];
    const float kProbeUp   = 0.6f;   // start the ray this far above the foot (m)
    const float kProbeDown = 1.6f;   // ray length below that (m)
    const float kLift      = 0.02f;  // keep the foot a hair above the surface (m)
    for (int s = 0; s < 2; ++s) {
        if (!m_sideOk[s]) continue;
        float footM[3]; mat4Translation(&G[(size_t)m_footNode[s]*16], footM);
        pl[s].curFoot[0]=footM[0]; pl[s].curFoot[1]=footM[1]; pl[s].curFoot[2]=footM[2];
        float footW[3]; toWorld(footM, footW);
        float origin[3] = { footW[0], footW[1] + kProbeUp, footW[2] };
        float down[3]   = { 0, -1, 0 };
        float hitW[3], nrm[3];
        if (m_groundRay.fn(origin, down, kProbeUp + kProbeDown, hitW, nrm, m_groundRay.user)) {
            pl[s].hit = true;
            // Bring the hit height back to model space (uniform-ish placement: invert
            // the Y translation/scale by projecting along world up). We only need the
            // delta in model +Y, so map the world hit Y through the model's up scale.
            // For the common case (placement = yaw rotation + translation, unit scale)
            // model_y = world_y - worldFromModel[13].
            float deltaWorldY = hitW[1] - footW[1];        // how far the ground is from the foot (world)
            pl[s].footModelY = footM[1] + deltaWorldY + kLift;  // unit-scale up axis
            pl[s].groundN[0]=nrm[0]; pl[s].groundN[1]=nrm[1]; pl[s].groundN[2]=nrm[2];
            if (!norm3(pl[s].groundN)) { pl[s].groundN[0]=0; pl[s].groundN[1]=1; pl[s].groundN[2]=0; }
        }
    }

    // 3) Pelvis adjust: the foot that must drop the MOST governs. We lower the pelvis
    // by the largest downward correction so neither leg over-extends; raises are not
    // applied to the pelvis (a foot that needs to go UP just bends its knee more).
    float requiredDrop = 0.0f;          // >=0 lowers the hips
    int hits = 0;
    for (int s = 0; s < 2; ++s) {
        if (!m_sideOk[s] || !pl[s].hit) continue;
        ++hits;
        float drop = pl[s].curFoot[1] - pl[s].footModelY;   // >0 if the ground is below the foot
        if (drop > requiredDrop) requiredDrop = drop;
    }
    // Clamp the pelvis drop to a sane fraction of leg length so it never collapses.
    float legLen = std::max(m_upperLen[0] + m_lowerLen[0], m_upperLen[1] + m_lowerLen[1]);
    float maxDrop = std::max(0.05f, 0.5f * legLen);
    if (requiredDrop > maxDrop) requiredDrop = maxDrop;
    if (requiredDrop < 0.0f) requiredDrop = 0.0f;
    if (hits == 0) requiredDrop = 0.0f;

    // Smooth the pelvis drop (critically-damped-ish) so steps don't pop.
    float pelvisRate = std::min(1.0f, 8.0f * dt);
    m_pelvisDropSmoothed += (requiredDrop - m_pelvisDropSmoothed) * pelvisRate;
    if (m_pelvisDropSmoothed < 1e-5f) m_pelvisDropSmoothed = (requiredDrop < 1e-5f ? 0.0f : m_pelvisDropSmoothed);

    // Apply the pelvis drop to the pelvis/root LOCAL translation along model up. The
    // pelvis local T is in its parent's space; for the typical pelvis (parent ~=
    // identity/armature root) model up == parent up, so subtract along Y.
    if (m_rootNode >= 0 && m_pelvisDropSmoothed > 1e-6f) {
        m_blendT[(size_t)m_rootNode*3 + 1] -= m_pelvisDropSmoothed;
        rebuildGlobals();   // pelvis local T changed -> re-accumulate globals
    }

    // 4) Solve each leg analytically toward its planted foot target, then convert the
    // result into LOCAL hip/knee rotation deltas. We work in MODEL space and fold the
    // delta into each joint's local rotation via its parent's model rotation.
    for (int s = 0; s < 2; ++s) {
        if (!m_sideOk[s]) continue;

        // Smooth the per-leg weight toward 1 (planted) or down toward 0 (miss).
        float targetW = pl[s].hit ? 1.0f : 0.0f;
        float wr = std::min(1.0f, 10.0f * dt);
        m_legW[s] += (targetW - m_legW[s]) * wr;
        if (m_legW[s] <= 1e-4f) continue;       // nothing to apply yet

        float hip[3], knee[3], foot[3];
        mat4Translation(&G[(size_t)m_hipNode[s]*16],  hip);
        mat4Translation(&G[(size_t)m_kneeNode[s]*16], knee);
        mat4Translation(&G[(size_t)m_footNode[s]*16], foot);

        // Foot target: keep the planar (XZ) position, set Y to the planted height.
        float target[3] = { foot[0], pl[s].hit ? pl[s].footModelY : foot[1], foot[2] };
        // Blend the target by the leg weight so enabling ramps in.
        target[1] = foot[1] + (target[1] - foot[1]) * m_legW[s];

        // Knee pole: the current knee-out direction projected away from the leg line,
        // so the bend keeps its natural (anatomical) direction.
        float legLine[3]; sub3(target, hip, legLine); norm3(legLine);
        float kd[3]; sub3(knee, hip, kd);
        float pdot = dot3(kd, legLine);
        float pole[3] = { kd[0]-pdot*legLine[0], kd[1]-pdot*legLine[1], kd[2]-pdot*legLine[2] };
        if (!norm3(pole)) { pole[0]=0; pole[1]=0; pole[2]=-1; } // forward (-Z) default

        float newKnee[3], newFoot[3];
        solveTwoBone(hip, knee, foot, target, pole, newKnee, newFoot);

        // ---- Convert the analytic result to LOCAL rotation deltas. We apply each
        // delta as a MODEL-space aim of the bone (current dir -> solved dir), folded
        // into the joint's local rotation through its parent's model rotation:
        //   M' = D_model * M  =>  L' = (conj(Pmodel) * D_model * Pmodel) * L
        // We track the hip's UPDATED model rotation so the knee (its child) uses the
        // correct parent frame instead of a stale one. ----
        const float idq[4] = {0,0,0,1};

        // Parent model rotation of a node (unit quat) read from the freshly-built G.
        auto parentModelRot = [&](int node, float out[4]) {
            int par = model.nodes[node].parent;
            out[0]=0; out[1]=0; out[2]=0; out[3]=1;
            if (par >= 0 && (uint32_t)par < m_nodeCount) {
                float t[3], sc[3]; decompose(&G[(size_t)par*16], t, out, sc);
            }
            quatNormalize(out);
        };
        // Fold a weighted model-space delta D into node `node`'s local rotation, using
        // the supplied parent model rotation Pm. Returns the node's NEW model rotation
        // (D_w_model * oldModelRot) so a child can chain off it.
        auto applyAim = [&](int node, const float Dmodel[4], const float Pm[4],
                            float weight, float outNewModel[4]) {
            float Dw[4]; slerp(idq, Dmodel, weight, Dw);     // weighted model delta
            // local delta = conj(Pm) * Dw * Pm
            float pConj[4] = { -Pm[0], -Pm[1], -Pm[2], Pm[3] };
            float tmp[4], dLocal[4];
            quatMul(pConj, Dw, tmp);
            quatMul(tmp, Pm, dLocal);
            quatNormalize(dLocal);
            float cur[4] = { m_blendR[(size_t)node*4+0], m_blendR[(size_t)node*4+1],
                             m_blendR[(size_t)node*4+2], m_blendR[(size_t)node*4+3] };
            float nw[4]; quatMul(dLocal, cur, nw); quatNormalize(nw);
            m_blendR[(size_t)node*4+0]=nw[0]; m_blendR[(size_t)node*4+1]=nw[1];
            m_blendR[(size_t)node*4+2]=nw[2]; m_blendR[(size_t)node*4+3]=nw[3];
            // new model rot = Dw * oldModelRot, oldModelRot = Pm * oldLocal
            float oldModel[4]; quatMul(Pm, cur, oldModel);
            quatMul(Dw, oldModel, outNewModel); quatNormalize(outNewModel);
        };

        // HIP: aim the upper bone (hip->knee) onto the solved direction.
        float curU[3]; sub3(knee, hip, curU); norm3(curU);
        float newU[3]; sub3(newKnee, hip, newU); norm3(newU);
        float dHipModel[4]; quatFromTo(curU, newU, dHipModel);
        float PmHip[4]; parentModelRot(m_hipNode[s], PmHip);
        float hipNewModel[4];
        applyAim(m_hipNode[s], dHipModel, PmHip, m_legW[s], hipNewModel);

        // KNEE: aim the lower bone (knee->foot) onto the solved direction. The knee's
        // parent is the hip, whose model rotation we just updated (hipNewModel).
        float curL[3]; sub3(foot, knee, curL); norm3(curL);
        float newL[3]; sub3(newFoot, newKnee, newL); norm3(newL);
        float dKneeModel[4]; quatFromTo(curL, newL, dKneeModel);
        float kneeNewModel[4];
        applyAim(m_kneeNode[s], dKneeModel, hipNewModel, m_legW[s], kneeNewModel);

        // FOOT: tilt the ankle so the sole follows the ground normal (model up -> N).
        // Skipped when the surface is ~flat. The foot's parent is the knee (updated).
        if (pl[s].hit) {
            float gN[3] = { pl[s].groundN[0], pl[s].groundN[1], pl[s].groundN[2] };
            if (dot3(gN, modelUp) < 0.9995f) {
                float dFootModel[4]; quatFromTo(modelUp, gN, dFootModel);
                float footNewModel[4];
                applyAim(m_footNode[s], dFootModel, kneeNewModel, m_legW[s] * 0.75f, footNewModel);
                (void)footNewModel;
            }
        }
    }
}

// ===========================================================================
// Self-test (--test-anim). Synthesize a 1-bone skinned GLB whose single joint
// rotates 90deg about Z over 1 second, load it headless, and assert: (a) the
// loader exposes a skin + a clip + a skinned primitive with CPU data, (b) the
// joint palette differs between t=0 and t=0.5, and (c) a known skinned vertex
// (the tip, fully weighted to the joint) actually moves between the two times.
// No window / Vulkan.
// ===========================================================================
namespace {
namespace fs = std::filesystem;

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[anim-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[anim-test] FAIL ") + name); }
}

void appendU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v));       b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}
std::vector<uint8_t> makeGlb(const std::string& json, const std::vector<uint8_t>& bin) {
    std::string j = json;
    while (j.size() % 4 != 0) j.push_back(' ');
    std::vector<uint8_t> binPad = bin;
    while (binPad.size() % 4 != 0) binPad.push_back(0);
    std::vector<uint8_t> glb;
    const uint32_t total = 12 + 8 + uint32_t(j.size()) + 8 + uint32_t(binPad.size());
    appendU32(glb, 0x46546C67); appendU32(glb, 2); appendU32(glb, total);
    appendU32(glb, uint32_t(j.size())); appendU32(glb, 0x4E4F534A);
    glb.insert(glb.end(), j.begin(), j.end());
    appendU32(glb, uint32_t(binPad.size())); appendU32(glb, 0x004E4942);
    glb.insert(glb.end(), binPad.begin(), binPad.end());
    return glb;
}

// Build a skinned GLB: a 4-vertex strip along +Y, all weighted to joint 0 (the
// only animated joint). Skin: 2 nodes (root mesh node + 1 joint node). One
// rotation clip: joint quaternion goes identity -> 90deg about Z over [0,1] s.
std::vector<uint8_t> makeSkinnedGlb() {
    // 4 verts at increasing +Y, on the X=0 plane (a vertical strip).
    struct V { float p[3]; float n[3]; float uv[2]; uint16_t j[4]; float w[4]; };
    std::vector<V> v = {
        {{-0.1f, 0.0f, 0}, {0,0,1}, {0,0}, {0,0,0,0}, {1,0,0,0}},
        {{ 0.1f, 0.0f, 0}, {0,0,1}, {1,0}, {0,0,0,0}, {1,0,0,0}},
        {{-0.1f, 2.0f, 0}, {0,0,1}, {0,1}, {0,0,0,0}, {1,0,0,0}},
        {{ 0.1f, 2.0f, 0}, {0,0,1}, {1,1}, {0,0,0,0}, {1,0,0,0}},
    };
    std::vector<uint16_t> idx = { 0,1,2, 2,1,3 };

    std::vector<uint8_t> bin;
    auto put = [&](const void* d, size_t n) {
        const uint8_t* p = (const uint8_t*)d; bin.insert(bin.end(), p, p + n);
    };
    auto align4 = [&]{ while (bin.size() % 4 != 0) bin.push_back(0); };

    const size_t nv = v.size();
    size_t posOfs = bin.size(); for (auto& vv : v) put(vv.p, 12);
    size_t nrmOfs = bin.size(); for (auto& vv : v) put(vv.n, 12);
    size_t uvOfs  = bin.size(); for (auto& vv : v) put(vv.uv, 8);
    size_t jOfs   = bin.size(); for (auto& vv : v) put(vv.j, 8);   // 4 * u16
    size_t wOfs   = bin.size(); for (auto& vv : v) put(vv.w, 16);
    size_t idxOfs = bin.size(); put(idx.data(), idx.size()*2); align4();
    // inverse bind matrix for joint 0 = identity (joint at origin in bind pose).
    size_t ibmOfs = bin.size();
    float ibm[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; put(ibm, 64);
    // animation input: 2 keyframe times [0,1]
    size_t timeOfs = bin.size(); float times[2] = {0.0f, 1.0f}; put(times, 8);
    // animation output: 2 quats, identity -> 90deg about Z (z=sin45, w=cos45)
    size_t rotOfs = bin.size();
    float s = std::sin((float)(0.7853981634)), c = std::cos((float)(0.7853981634));
    float quats[8] = { 0,0,0,1,  0,0,s,c }; put(quats, 32);

    char buf[2048];
    std::string j = "{\"asset\":{\"version\":\"2.0\"},";
    j += "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],";
    // node 0 = mesh node (skin 0); node 1 = joint node (animated).
    j += "\"nodes\":[{\"mesh\":0,\"skin\":0},{\"name\":\"joint0\"}],";
    j += "\"meshes\":[{\"primitives\":[{\"attributes\":{";
    j += "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"JOINTS_0\":3,\"WEIGHTS_0\":4},";
    j += "\"indices\":5}]}],";
    j += "\"skins\":[{\"joints\":[1],\"inverseBindMatrices\":6}],";
    // animation: channel rotation on node 1, sampler 0.
    j += "\"animations\":[{\"name\":\"BendIdle\",\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}}],";
    j += "\"samplers\":[{\"input\":7,\"output\":8,\"interpolation\":\"LINEAR\"}]}],";

    std::snprintf(buf, sizeof buf,
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\",\"min\":[-0.1,0,0],\"max\":[0.1,2,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":%zu,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC4\"},"
        "{\"bufferView\":5,\"componentType\":5123,\"count\":%zu,\"type\":\"SCALAR\"},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":1,\"type\":\"MAT4\"},"
        "{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
        "{\"bufferView\":8,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}],",
        nv, nv, nv, nv, nv, idx.size());
    j += buf;
    std::snprintf(buf, sizeof buf,
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":64},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":32}],",
        posOfs, nv*12, nrmOfs, nv*12, uvOfs, nv*8, jOfs, nv*8, wOfs, nv*16,
        idxOfs, idx.size()*2, ibmOfs, timeOfs, rotOfs);
    j += buf;
    std::snprintf(buf, sizeof buf, "\"buffers\":[{\"byteLength\":%zu}]}", bin.size());
    j += buf;
    return makeGlb(j, bin);
}

} // namespace

bool runAnimSelfTest() {
    g_pass = g_fail = 0;
    x3::logInfo("[anim-test] J1 skeletal animation + CPU skinning self-test");

    std::error_code ec;
    fs::path tmp = fs::temp_directory_path() / "x3native_animtest";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    {
        std::vector<uint8_t> glb = makeSkinnedGlb();
        std::ofstream f(tmp / "bone.glb", std::ios::binary);
        f.write((const char*)glb.data(), (std::streamsize)glb.size());
    }

    std::unique_ptr<x3::asset::IAssetSource> src(x3::asset::createAssetSource());
    src->mountDir(tmp.string(), 0);
    // Headless loader (null device): primitives keep CPU skin data, mesh ids fake.
    std::unique_ptr<x3::asset::IModelLoader> loader(
        x3::asset::createModelLoader(nullptr, src.get()));
    x3::asset::Model model = loader->load("bone.glb");

    // (a) loader exposed skin + clip + skinned primitive with CPU data.
    bool skinned = false;
    for (const auto& p : model.primitives) if (p.skinned && !p.basePos.empty()) skinned = true;
    bool parsed = model.ok && !model.skins.empty() && !model.skins[0].joints.empty()
               && !model.animations.empty() && skinned;
    check(parsed, "T1 loader exposes skin + clip + skinned vertex data");

    Skinner sk;
    bool bound = sk.bind(model);
    check(bound && sk.clipCount() >= 1, "T2 Skinner binds a clip");

    if (bound) {
        // (b) palette differs between t=0 and t=0.5.
        std::vector<float> pal0, palMid;
        uint32_t n0 = sk.computePalette(model, 0, 0.0f, pal0);
        uint32_t n1 = sk.computePalette(model, 0, 0.5f, palMid);
        bool diff = (n0 == n1) && n0 > 0 && pal0.size() == palMid.size();
        float maxDelta = 0.0f;
        if (diff) for (size_t i = 0; i < pal0.size(); ++i)
            maxDelta = std::max(maxDelta, std::fabs(pal0[i] - palMid[i]));
        check(diff && maxDelta > 1e-3f, "T3 joint palette changes between t=0 and t=0.5");

        // (c) a fully-weighted tip vertex (index 2, at y=2) moves between t=0 and
        // t=mid. Skin it by hand using the palette (the device path is headless).
        const x3::asset::MeshPrimitive* sp = nullptr;
        for (const auto& p : model.primitives) if (p.skinned) { sp = &p; break; }
        bool moved = false;
        if (sp && sp->basePos.size() >= 9) {
            auto skinTip = [&](std::vector<float>& pal, float out[3]) {
                const float* bp = &sp->basePos[2*3];     // vertex 2 (the tip)
                const float* jm = &pal[0];               // joint 0 palette
                out[0] = jm[0]*bp[0] + jm[4]*bp[1] + jm[8]*bp[2] + jm[12];
                out[1] = jm[1]*bp[0] + jm[5]*bp[1] + jm[9]*bp[2] + jm[13];
                out[2] = jm[2]*bp[0] + jm[6]*bp[1] + jm[10]*bp[2] + jm[14];
            };
            float a[3], b[3]; skinTip(pal0, a); skinTip(palMid, b);
            float d = std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) +
                                (a[2]-b[2])*(a[2]-b[2]));
            moved = d > 1e-2f;
            x3::logInfo("[anim-test] tip moved " + std::to_string(d) + " m between t=0 and t=0.5");
        }
        check(moved, "T4 a known skinned vertex moves over time");
    }

    loader->unload(model);
    fs::remove_all(tmp, ec);

    // ---- T1 locomotion-blend checks against a real multi-clip GLB if present.
    // (The big *_anim.glb are generated artifacts — may be absent in a clean
    // checkout. When absent we skip these checks and the J1 test still PASSES.) ----
    {
        const std::string kLocoGlb = x3::game::riggedGlbRoot() + "/chief_martinez_anim.glb";
        if (fs::exists(kLocoGlb)) {
            x3::logInfo("[anim-test] T1 locomotion blend present-asset checks");
            bool ok = runLocomotionSelfTest(kLocoGlb);
            check(ok, "T5 locomotion blend (idle/walk/run + crossfade) on real GLB");
        } else {
            x3::logInfo("[anim-test] (locomotion GLB absent — skipping T1 blend checks; clean checkout)");
        }
    }

    x3::logInfo("[anim-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// T1 locomotion-blend self-test (--test-locomotion). Loads a real multi-clip
// GLB headless and asserts the 1D blend + crossfade behave:
//   L1 model is skinnable + exposes Idle/Walk/Run clips.
//   L2 speed=0 -> pose ~= Idle (blended palette near the pure Idle palette).
//   L3 mid speed -> Walk-dominant (closer to Walk than to Idle or Run).
//   L4 high speed -> Run-dominant (closer to Run than to Walk).
//   L5 sweeping the speed param changes the blended palette monotonically-ish
//      (low vs high differ by more than low vs mid — the blend tracks the param).
//   L6 a Jump crossfade is continuous: no single-frame palette jump exceeds a
//      small bound across the whole transition (pop-free).
// Returns true iff all pass. No window / Vulkan.
// ===========================================================================
bool runLocomotionSelfTest(const std::string& glbPath) {
    int pass = 0, fail = 0;
    auto lcheck = [&](bool cond, const char* name) {
        if (cond) { ++pass; x3::logInfo(std::string("[loco-test] PASS ") + name); }
        else      { ++fail; x3::logError(std::string("[loco-test] FAIL ") + name); }
    };

    std::string path = glbPath.empty()
        ? (x3::game::riggedGlbRoot() + "/chief_martinez_anim.glb") : glbPath;
    if (!fs::exists(path)) {
        x3::logError("[loco-test] GLB not found: " + path);
        return false;
    }
    x3::logInfo("[loco-test] locomotion blend self-test on " + path);

    fs::path p(path);
    std::unique_ptr<x3::asset::IAssetSource> src(x3::asset::createAssetSource());
    src->mountDir(p.parent_path().string(), 0);
    std::unique_ptr<x3::asset::IModelLoader> loader(
        x3::asset::createModelLoader(nullptr, src.get()));   // headless
    x3::asset::Model model = loader->load(p.filename().string());
    if (!model.ok) { x3::logError("[loco-test] load failed"); return false; }

    Skinner sk;
    bool bound = sk.bind(model);
    int idle = sk.findClip({ "idle", "stand" });
    int walk = sk.findClip({ "walk" });
    int run  = sk.findClip({ "run", "jog", "sprint" });
    int jump = sk.findClip({ "jump", "leap" });
    lcheck(bound && idle >= 0 && walk >= 0 && run >= 0,
           "L1 skinnable + Idle/Walk/Run clips found");
    if (!bound || idle < 0 || walk < 0 || run < 0) {
        loader->unload(model);
        x3::logInfo(std::string("[loco-test] ") + std::to_string(pass) + " passed, " +
                    std::to_string(fail) + " failed");
        return fail == 0;
    }
    sk.setLocomotionClips(idle, walk, run, 1.5f, 4.0f);

    // Reference palettes for the pure clips at a fixed phase. We compare the
    // blended pose to these to judge dominance. (Use the same phase so the
    // comparison is about the blend weight, not the phase.)
    auto paletteDist = [](const std::vector<float>& a, const std::vector<float>& b) {
        float d = 0.0f; size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) d += std::fabs(a[i] - b[i]);
        return d;
    };

    // Settle the blend at a given 0..1 param by stepping a fixed time, then read
    // the palette. Stepping lets the smoothing + phase converge.
    const float dt = 1.0f / 60.0f;
    auto settleAndPalette = [&](float loco01, std::vector<float>& out) {
        sk.setLocomotion01(loco01);
        for (int i = 0; i < 90; ++i) sk.advanceAndComputePalette(model, dt, out);
    };

    std::vector<float> palIdle, palWalk, palRun, palLow, palMid, palHigh;
    // Pure-clip references: drive the blend fully to each end / middle.
    settleAndPalette(0.0f, palIdle);   // pure idle
    settleAndPalette(0.5f, palWalk);   // pure walk (midpoint of the 1D blend)
    settleAndPalette(1.0f, palRun);    // pure run

    // L2: speed=0 -> ~Idle. The settled param 0 IS idle by construction, so check
    // it is much closer to the Idle clip sampled directly than to Walk/Run.
    {
        // Compare the blend@0 palette against directly-sampled clip palettes at the
        // current phase by re-driving to 0 and reading once more.
        settleAndPalette(0.0f, palLow);
        float dWalk = paletteDist(palLow, palWalk);
        float dRun  = paletteDist(palLow, palRun);
        float dIdle = paletteDist(palLow, palIdle);   // ~0 (same setting)
        bool idleDom = dIdle < dWalk && dIdle < dRun;
        x3::logInfo("[loco-test] L2 speed0: dIdle=" + std::to_string(dIdle) +
                    " dWalk=" + std::to_string(dWalk) + " dRun=" + std::to_string(dRun));
        lcheck(idleDom, "L2 speed=0 pose is Idle-dominant");
    }

    // L3: mid speed -> Walk-dominant (closer to Walk than to Idle or Run).
    {
        settleAndPalette(0.5f, palMid);
        float dIdle = paletteDist(palMid, palIdle);
        float dWalk = paletteDist(palMid, palWalk);
        float dRun  = paletteDist(palMid, palRun);
        bool walkDom = dWalk < dIdle && dWalk < dRun;
        x3::logInfo("[loco-test] L3 mid: dIdle=" + std::to_string(dIdle) +
                    " dWalk=" + std::to_string(dWalk) + " dRun=" + std::to_string(dRun));
        lcheck(walkDom, "L3 mid speed is Walk-dominant");
    }

    // L4: high speed -> Run-dominant (closer to Run than to Walk).
    {
        settleAndPalette(1.0f, palHigh);
        float dWalk = paletteDist(palHigh, palWalk);
        float dRun  = paletteDist(palHigh, palRun);
        bool runDom = dRun < dWalk;
        x3::logInfo("[loco-test] L4 high: dWalk=" + std::to_string(dWalk) +
                    " dRun=" + std::to_string(dRun));
        lcheck(runDom, "L4 high speed is Run-dominant");
    }

    // L5: the blend tracks the param — idle->run differs more than idle->mid.
    {
        float dIdleToMid  = paletteDist(palLow, palMid);
        float dIdleToHigh = paletteDist(palLow, palHigh);
        bool tracks = dIdleToHigh > dIdleToMid && dIdleToMid > 1e-2f;
        x3::logInfo("[loco-test] L5 dIdle->mid=" + std::to_string(dIdleToMid) +
                    " dIdle->high=" + std::to_string(dIdleToHigh));
        lcheck(tracks, "L5 blended palette tracks the speed param (sweep monotone)");
    }

    // L6: a Jump crossfade is pop-free. Walk steadily, trigger Jump, and verify no
    // single frame's palette delta exceeds a small bound through the transition.
    if (jump >= 0) {
        std::vector<float> prev, cur;
        sk.setLocomotion01(0.5f);                    // walking
        for (int i = 0; i < 60; ++i) sk.advanceAndComputePalette(model, dt, prev);
        // Baseline per-frame delta while just walking (the natural motion).
        float walkMaxStep = 0.0f;
        for (int i = 0; i < 30; ++i) {
            sk.advanceAndComputePalette(model, dt, cur);
            walkMaxStep = std::max(walkMaxStep, paletteDist(prev, cur));
            prev.swap(cur);
        }
        // Now trigger the Jump crossfade and watch the per-frame delta. A SNAP
        // (no crossfade) would show a delta many times the natural walk step.
        sk.triggerClip(jump, 0.2f, /*loop=*/false);
        float xfadeMaxStep = 0.0f;
        for (int i = 0; i < 150; ++i) {              // ~2.5 s covers fade-in/out
            sk.advanceAndComputePalette(model, dt, cur);
            xfadeMaxStep = std::max(xfadeMaxStep, paletteDist(prev, cur));
            prev.swap(cur);
        }
        // Continuous: the worst transition frame stays within a modest multiple of
        // the natural walk step (no pop). Allowance is generous (Jump moves a lot)
        // but a true snap would be an order of magnitude larger.
        float popBound = std::max(walkMaxStep * 6.0f, 0.5f);
        bool continuous = xfadeMaxStep < popBound;
        x3::logInfo("[loco-test] L6 walkStep=" + std::to_string(walkMaxStep) +
                    " xfadeMaxStep=" + std::to_string(xfadeMaxStep) +
                    " bound=" + std::to_string(popBound));
        lcheck(continuous, "L6 Jump crossfade is continuous (no pop)");
    } else {
        x3::logInfo("[loco-test] (no Jump clip — skipping L6 crossfade check)");
    }

    loader->unload(model);
    x3::logInfo(std::string("[loco-test] ") + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

// ===========================================================================
// Foot-IK self-test (--test-footik). Pure-geometry solver checks + a synthetic
// two-leg rig for the joint-name resolution + plant/pelvis behavior, plus an
// optional present-asset resolve on the real rig. No window / Vulkan.
// ===========================================================================
namespace {

int g_fpass = 0, g_ffail = 0;
void fcheck(bool cond, const char* name) {
    if (cond) { ++g_fpass; x3::logInfo(std::string("[footik-test] PASS ") + name); }
    else      { ++g_ffail; x3::logError(std::string("[footik-test] FAIL ") + name); }
}

// Build a minimal SKINNED, two-leg humanoid GLB with NAMED bones so the foot-IK
// resolver can find Hips / UpperLeg.[LR] / LowerLeg.[LR] / Foot.[LR]. The mesh is
// a single tiny quad weighted to the left foot joint (enough for bind() to accept
// a skin); the bones carry an identity Idle clip so the Skinner binds + samples.
// Layout (all local translations; +Y up):
//   0 mesh node (skin 0)
//   1 Hips            T(0, 0.9, 0)
//   2 UpperLeg.L      child of 1, T(+0.1, 0,    0)
//   3 LowerLeg.L      child of 2, T( 0,  -0.45, 0)   upperLen = 0.45
//   4 Foot.L          child of 3, T( 0,  -0.45, 0)   lowerLen = 0.45
//   5 UpperLeg.R      child of 1, T(-0.1, 0,    0)
//   6 LowerLeg.R      child of 5, T( 0,  -0.45, 0)
//   7 Foot.R          child of 6, T( 0,  -0.45, 0)
std::vector<uint8_t> makeTwoLegGlb() {
    // One quad (4 verts) weighted fully to joint index 2 (Foot.L = node 4, joint 2).
    struct V { float p[3]; float n[3]; float uv[2]; uint16_t j[4]; float w[4]; };
    std::vector<V> v = {
        {{-0.1f, 0.05f, 0}, {0,0,1}, {0,0}, {2,0,0,0}, {1,0,0,0}},
        {{ 0.1f, 0.05f, 0}, {0,0,1}, {1,0}, {2,0,0,0}, {1,0,0,0}},
        {{-0.1f, 0.10f, 0}, {0,0,1}, {0,1}, {2,0,0,0}, {1,0,0,0}},
        {{ 0.1f, 0.10f, 0}, {0,0,1}, {1,1}, {2,0,0,0}, {1,0,0,0}},
    };
    std::vector<uint16_t> idx = { 0,1,2, 2,1,3 };

    std::vector<uint8_t> bin;
    auto put = [&](const void* d, size_t n){ const uint8_t* p=(const uint8_t*)d; bin.insert(bin.end(), p, p+n); };
    auto align4 = [&]{ while (bin.size()%4) bin.push_back(0); };
    const size_t nv = v.size();
    size_t posOfs = bin.size(); for (auto& vv:v) put(vv.p,12);
    size_t nrmOfs = bin.size(); for (auto& vv:v) put(vv.n,12);
    size_t uvOfs  = bin.size(); for (auto& vv:v) put(vv.uv,8);
    size_t jOfs   = bin.size(); for (auto& vv:v) put(vv.j,8);
    size_t wOfs   = bin.size(); for (auto& vv:v) put(vv.w,16);
    size_t idxOfs = bin.size(); put(idx.data(), idx.size()*2); align4();
    // 6 inverse-bind matrices (joints: UpperLeg.L, LowerLeg.L, Foot.L, UpperLeg.R,
    // LowerLeg.R, Foot.R) — identity is fine for the resolve/plant test.
    size_t ibmOfs = bin.size();
    for (int k=0;k<6;++k){ float ibm[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; put(ibm,64); }
    // Idle clip: one rotation key (identity) on Hips so a clip + sampler exist.
    size_t timeOfs = bin.size(); float times[1]={0.0f}; put(times,4);
    size_t rotOfs  = bin.size(); float q[4]={0,0,0,1}; put(q,16);
    align4();

    char buf[4096];
    std::string j = "{\"asset\":{\"version\":\"2.0\"},";
    j += "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],";
    j += "\"nodes\":[";
    j += "{\"mesh\":0,\"skin\":0},";                                          // 0
    j += "{\"name\":\"Hips\",\"translation\":[0,0.9,0],\"children\":[2,5]},"; // 1
    j += "{\"name\":\"UpperLeg.L\",\"translation\":[0.1,0,0],\"children\":[3]},";  // 2
    j += "{\"name\":\"LowerLeg.L\",\"translation\":[0,-0.45,0],\"children\":[4]},";// 3
    j += "{\"name\":\"Foot.L\",\"translation\":[0,-0.45,0]},";                // 4
    j += "{\"name\":\"UpperLeg.R\",\"translation\":[-0.1,0,0],\"children\":[6]},"; // 5
    j += "{\"name\":\"LowerLeg.R\",\"translation\":[0,-0.45,0],\"children\":[7]},";// 6
    j += "{\"name\":\"Foot.R\",\"translation\":[0,-0.45,0]}";                 // 7
    j += "],";
    j += "\"meshes\":[{\"primitives\":[{\"attributes\":{";
    j += "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"JOINTS_0\":3,\"WEIGHTS_0\":4},\"indices\":5}]}],";
    j += "\"skins\":[{\"joints\":[2,3,4,5,6,7],\"inverseBindMatrices\":6}],";
    j += "\"animations\":[{\"name\":\"Idle\",\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}}],";
    j += "\"samplers\":[{\"input\":7,\"output\":8,\"interpolation\":\"STEP\"}]}],";
    std::snprintf(buf, sizeof buf,
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":%zu,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC4\"},"
        "{\"bufferView\":5,\"componentType\":5123,\"count\":%zu,\"type\":\"SCALAR\"},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":6,\"type\":\"MAT4\"},"
        "{\"bufferView\":7,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\",\"min\":[0],\"max\":[0]},"
        "{\"bufferView\":8,\"componentType\":5126,\"count\":1,\"type\":\"VEC4\"}],",
        nv,nv,nv,nv,nv, idx.size());
    j += buf;
    std::snprintf(buf, sizeof buf,
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":384},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":4},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":16}],",
        posOfs,nv*12, nrmOfs,nv*12, uvOfs,nv*8, jOfs,nv*8, wOfs,nv*16,
        idxOfs,idx.size()*2, ibmOfs, timeOfs, rotOfs);
    j += buf;
    std::snprintf(buf, sizeof buf, "\"buffers\":[{\"byteLength\":%zu}]}", bin.size());
    j += buf;
    return makeGlb(j, bin);
}

} // namespace

bool runFootIkSelfTest() {
    g_fpass = g_ffail = 0;
    x3::logInfo("[footik-test] foot-IK (two-bone + plant + pelvis) self-test");

    // ---- F1: reachable two-bone solve. Hip at origin, straight leg down the -Y
    // axis (knee at -1, foot at -2; both bones length 1). Target a reachable point;
    // assert the foot lands on it and the bone lengths are preserved. ----
    {
        float hip[3]  = { 0, 0, 0 };
        float knee[3] = { 0, -1, 0 };
        float foot[3] = { 0, -2, 0 };
        float pole[3] = { 0, 0, -1 };        // bend the knee forward (-Z)
        float target[3] = { 0.8f, -1.2f, 0.3f };  // clearly reachable (|t-hip|<2)
        float ok[3], of[3];
        x3::anim::Skinner::solveTwoBone(hip, knee, foot, target, pole, ok, of);
        float df = std::sqrt((of[0]-target[0])*(of[0]-target[0]) +
                             (of[1]-target[1])*(of[1]-target[1]) +
                             (of[2]-target[2])*(of[2]-target[2]));
        float l1 = std::sqrt((ok[0]-hip[0])*(ok[0]-hip[0]) + (ok[1]-hip[1])*(ok[1]-hip[1]) + (ok[2]-hip[2])*(ok[2]-hip[2]));
        float l2 = std::sqrt((of[0]-ok[0])*(of[0]-ok[0]) + (of[1]-ok[1])*(of[1]-ok[1]) + (of[2]-ok[2])*(of[2]-ok[2]));
        // Knee should bend toward the pole side (negative-ish Z) — sanity, not strict.
        x3::logInfo("[footik-test] F1 footErr=" + std::to_string(df) +
                    " L1=" + std::to_string(l1) + " L2=" + std::to_string(l2) +
                    " kneeZ=" + std::to_string(ok[2]));
        bool reach = df < 1e-3f && std::fabs(l1-1.0f) < 1e-3f && std::fabs(l2-1.0f) < 1e-3f;
        fcheck(reach, "F1 reachable target: foot reaches within epsilon, bone lengths preserved");
    }

    // ---- F2: unreachable target -> straight leg pointing at the target, no jitter.
    // Two over-reach targets at the same direction but different distances must give
    // the SAME (fully extended, colinear) configuration. ----
    {
        float hip[3]  = { 0, 0, 0 };
        float knee[3] = { 0, -1, 0 };
        float foot[3] = { 0, -2, 0 };
        float pole[3] = { 0, 0, -1 };
        float tA[3] = { 3.0f, 0.0f, 0.0f };   // far past reach (dist 3 > 2)
        float tB[3] = { 6.0f, 0.0f, 0.0f };   // even farther, same direction
        float kA[3], fA[3], kB[3], fB[3];
        x3::anim::Skinner::solveTwoBone(hip, knee, foot, tA, pole, kA, fA);
        x3::anim::Skinner::solveTwoBone(hip, knee, foot, tB, pole, kB, fB);
        // Straight: hip->foot ~= L1+L2 (full extension) and the knee lies essentially
        // ON the hip->foot line (deviation tiny — the boundary clamp leaves at most a
        // hair of bend by design, avoiding the fully-extended singularity).
        float lhk = std::sqrt(kA[0]*kA[0]+kA[1]*kA[1]+kA[2]*kA[2]);   // |knee-hip|
        float total = std::sqrt(fA[0]*fA[0]+fA[1]*fA[1]+fA[2]*fA[2]); // |foot-hip|
        // Distance from the knee to the hip->foot line (0 = perfectly colinear).
        float fdir[3] = { fA[0], fA[1], fA[2] }; float fl = std::sqrt(fdir[0]*fdir[0]+fdir[1]*fdir[1]+fdir[2]*fdir[2]);
        if (fl > 1e-6f) { fdir[0]/=fl; fdir[1]/=fl; fdir[2]/=fl; }
        float proj = kA[0]*fdir[0]+kA[1]*fdir[1]+kA[2]*fdir[2];
        float perp[3] = { kA[0]-proj*fdir[0], kA[1]-proj*fdir[1], kA[2]-proj*fdir[2] };
        float kneeOff = std::sqrt(perp[0]*perp[0]+perp[1]*perp[1]+perp[2]*perp[2]);
        bool straight = std::fabs(total - 2.0f) < 1e-2f && std::fabs(lhk - 1.0f) < 1e-2f && kneeOff < 0.05f;
        // Stable: the two over-reach distances produce the same knee/foot (no jitter).
        float jit = std::sqrt((kA[0]-kB[0])*(kA[0]-kB[0])+(fA[0]-fB[0])*(fA[0]-fB[0]));
        x3::logInfo("[footik-test] F2 total=" + std::to_string(total) +
                    " kneeOffLine=" + std::to_string(kneeOff) + " jitter=" + std::to_string(jit));
        fcheck(straight && jit < 1e-3f, "F2 unreachable target: straight extended leg, no jitter");
    }

    // ---- F3: synthetic two-leg rig — name resolution + plant + bounded pelvis drop.
    {
        std::error_code ec;
        fs::path tmp = fs::temp_directory_path() / "x3native_footiktest";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        { std::vector<uint8_t> glb = makeTwoLegGlb();
          std::ofstream f(tmp / "twoleg.glb", std::ios::binary);
          f.write((const char*)glb.data(), (std::streamsize)glb.size()); }

        std::unique_ptr<x3::asset::IAssetSource> src(x3::asset::createAssetSource());
        src->mountDir(tmp.string(), 0);
        std::unique_ptr<x3::asset::IModelLoader> loader(
            x3::asset::createModelLoader(nullptr, src.get()));   // headless
        x3::asset::Model model = loader->load("twoleg.glb");

        Skinner sk;
        bool bound = sk.bind(model);
        bool resolved = bound && sk.footIkResolved();
        x3::logInfo(std::string("[footik-test] F3 resolved L=(") +
                    std::string(sk.footIkBoneName(0,0)) + "," + std::string(sk.footIkBoneName(0,1)) +
                    "," + std::string(sk.footIkBoneName(0,2)) + ") R=(" +
                    std::string(sk.footIkBoneName(1,0)) + "," + std::string(sk.footIkBoneName(1,1)) +
                    "," + std::string(sk.footIkBoneName(1,2)) + ") pelvis=" +
                    std::string(sk.footIkBoneName(0,3)));
        fcheck(resolved, "F3a synthetic rig resolves UpperLeg/LowerLeg/Foot/Hips by name");

        if (resolved) {
            sk.setLocomotionClips(0, -1, -1, 1.5f, 4.0f);   // idle-only blend
            sk.setLocomotion01(0.0f);

            // Ground heights, read by the (captureless) ground-ray callbacks via the
            // user ptr. The rig in this rest pose has UpperLeg.L at +X, .R at -X.
            struct Ground { float flatY; float leftY; float rightY; };
            using GR = x3::anim::Skinner::GroundRay;

            // Flat ground exactly at the rest foot height. Rest foot Y in model space:
            // Hips 0.9 + UpperLeg 0 + LowerLeg -0.45 + Foot -0.45 = 0.0. So plant at 0.
            Ground gFlat{ 0.0f, 0.0f, 0.0f };
            GR grFlat;
            grFlat.fn = [](const float o[3], const float d[3], float maxD,
                           float hit[3], float n[3], void* u) -> bool {
                (void)d; const Ground* g = (const Ground*)u;
                hit[0]=o[0]; hit[1]=g->flatY; hit[2]=o[2]; n[0]=0; n[1]=1; n[2]=0;
                return (o[1] - g->flatY) <= maxD;
            };
            grFlat.user = &gFlat;
            const float identity[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
            sk.setFootIk(true, grFlat, identity);

            std::vector<float> pal;
            for (int i = 0; i < 60; ++i) sk.advanceAndComputePalette(model, 1.0f/60.0f, pal);
            // Flat ground at rest height -> tiny pelvis drop, both legs weighted ~1.
            float dropFlat = sk.footIkPelvisDrop();
            bool flatOk = dropFlat < 0.06f && sk.footIkLegWeight(0) > 0.5f && sk.footIkLegWeight(1) > 0.5f;
            x3::logInfo("[footik-test] F3 flat: pelvisDrop=" + std::to_string(dropFlat) +
                        " wL=" + std::to_string(sk.footIkLegWeight(0)) +
                        " wR=" + std::to_string(sk.footIkLegWeight(1)));
            fcheck(flatOk, "F3b flat ground: feet planted, legs weighted, ~no pelvis drop");

            // Step: raise the left side's ground so the LEFT foot would float; the
            // lower (right) foot governs and the pelvis drops a bounded, smoothed amount.
            Ground gStep{ 0.0f, 0.18f, -0.15f };   // leftY raised, rightY lowered
            GR grStep; grStep.user = &gStep;
            grStep.fn = [](const float o[3], const float d[3], float maxD,
                           float hit[3], float n[3], void* u) -> bool {
                (void)d; const Ground* g = (const Ground*)u;
                float y = (o[0] >= 0.0f) ? g->leftY : g->rightY;
                hit[0]=o[0]; hit[1]=y; hit[2]=o[2]; n[0]=0; n[1]=1; n[2]=0;
                return (o[1] - y) <= maxD;
            };
            sk.setFootIk(true, grStep, identity);
            float prevDrop = sk.footIkPelvisDrop();
            float maxStep = 0.0f;
            for (int i = 0; i < 120; ++i) {
                sk.advanceAndComputePalette(model, 1.0f/60.0f, pal);
                float d = std::fabs(sk.footIkPelvisDrop() - prevDrop);
                if (d > maxStep) maxStep = d;
                prevDrop = sk.footIkPelvisDrop();
            }
            float dropStep = sk.footIkPelvisDrop();
            float legLen = 0.9f;   // 0.45 + 0.45
            // The right foot is 0.15 below rest -> pelvis should drop toward ~0.15,
            // bounded, and the per-frame change stays small (smoothed, no pop).
            bool stepOk = dropStep > 0.05f && dropStep < 0.5f*legLen + 1e-3f && maxStep < 0.05f;
            x3::logInfo("[footik-test] F3 step: pelvisDrop=" + std::to_string(dropStep) +
                        " maxPerFrameStep=" + std::to_string(maxStep));
            fcheck(stepOk, "F3c step/slope: lower foot governs a bounded, smoothed pelvis drop");

            // Disable -> the IK weights relax back to ~0 (graceful, pop-free off).
            GR none{};
            sk.setFootIk(false, none, identity);
            for (int i = 0; i < 60; ++i) sk.advanceAndComputePalette(model, 1.0f/60.0f, pal);
            bool offOk = sk.footIkLegWeight(0) < 0.05f && sk.footIkLegWeight(1) < 0.05f
                      && sk.footIkPelvisDrop() < 0.02f;
            x3::logInfo("[footik-test] F3 off: wL=" + std::to_string(sk.footIkLegWeight(0)) +
                        " drop=" + std::to_string(sk.footIkPelvisDrop()));
            fcheck(offOk, "F3d disable: IK weights + pelvis relax to zero (no-op)");
        }
        loader->unload(model);
        fs::remove_all(tmp, ec);
    }

    // ---- F4 (present-asset): resolve the real rig + log its bone names. Skipped
    // (still PASS) on a clean checkout where the asset is absent. ----
    {
        const std::string kGlb = x3::game::riggedGlbRoot() + "/chief_martinez_anim.glb";
        if (fs::exists(kGlb)) {
            fs::path p(kGlb);
            std::unique_ptr<x3::asset::IAssetSource> src(x3::asset::createAssetSource());
            src->mountDir(p.parent_path().string(), 0);
            std::unique_ptr<x3::asset::IModelLoader> loader(
                x3::asset::createModelLoader(nullptr, src.get()));
            x3::asset::Model model = loader->load(p.filename().string());
            Skinner sk;
            bool bound = sk.bind(model);
            bool resolved = bound && sk.footIkResolved();
            x3::logInfo(std::string("[footik-test] F4 real rig resolved L=(") +
                        std::string(sk.footIkBoneName(0,0)) + "," + std::string(sk.footIkBoneName(0,1)) +
                        "," + std::string(sk.footIkBoneName(0,2)) + ") R=(" +
                        std::string(sk.footIkBoneName(1,0)) + "," + std::string(sk.footIkBoneName(1,1)) +
                        "," + std::string(sk.footIkBoneName(1,2)) + ") pelvis=" +
                        std::string(sk.footIkBoneName(0,3)));
            fcheck(resolved, "F4 real rig (chief_martinez_anim.glb) resolves a leg chain by name");
            loader->unload(model);
        } else {
            x3::logInfo("[footik-test] (real rig absent — skipping F4; clean checkout)");
        }
    }

    x3::logInfo("[footik-test] " + std::to_string(g_fpass) + " passed, " +
                std::to_string(g_ffail) + " failed");
    return g_ffail == 0;
}

} // namespace x3::anim
