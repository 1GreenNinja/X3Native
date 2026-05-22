// Skeletal animation + CPU skinning runtime (J1). See app/anim.h.
//
// Clean-room: glTF 2.0 animation sampling + linear-blend skinning math built from
// the public spec only; no GPL / id Tech / RBDOOM source consulted. Column-major
// 4x4 (glTF/glm convention) throughout, matching the M2 loader's node transforms.

#include "anim.h"
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
    m_valid = true;
    return true;
}

float Skinner::clipDuration(uint32_t clip) const {
    return clip < m_clipDurations.size() ? m_clipDurations[clip] : 0.0f;
}
std::string_view Skinner::clipName(uint32_t clip) const {
    return clip < m_clipNames.size() ? std::string_view(m_clipNames[clip]) : std::string_view{};
}

int Skinner::findClip(std::initializer_list<const char*> keys) const {
    for (const char* key : keys) {
        std::string k = toLower(key);
        for (uint32_t c = 0; c < (uint32_t)m_clipNames.size(); ++c) {
            if (toLower(m_clipNames[c]).find(k) != std::string::npos) return (int)c;
        }
    }
    return -1;
}

void Skinner::sampleNodeLocal(const x3::asset::Model& m, uint32_t clip, int node,
                              float t, float out[16]) const {
    const x3::asset::Node& nd = m.nodes[node];
    // Bind-pose fallback TRS.
    float T[3], R[4], S[3];
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

void Skinner::apply(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
                    uint32_t clip, float timeSec) {
    if (!m_valid) return;
    uint32_t jcount = computePalette(model, clip, timeSec, m_palette);
    if (jcount == 0) return;

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

    x3::logInfo("[anim-test] " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::anim
