#pragma once
// Skeletal animation + CPU skinning runtime (J1). See app/anim.cpp.
//
// Clean-room: built only from the engine's own IModelLoader Model data (skins /
// nodes / animation clips parsed by the M2 cgltf loader), public glTF 2.0 +
// linear-blend-skinning math, and the IRenderDevice::updateMesh re-upload hook.
// No GPL / id Tech / RBDOOM source consulted.
//
// WHAT IT DOES
//   Given a loaded skinned Model + an active clip + a playback time, it:
//     1. samples each animated node's local Translation/Rotation/Scale from the
//        clip (LINEAR for T/S, SLERP for rotation quats), falling back to the
//        node's bind-pose TRS for un-animated channels;
//     2. composes global node matrices down the hierarchy;
//     3. builds jointMatrix[j] = globalNode[joint[j]] * inverseBind[j];
//     4. CPU-skins every skinned primitive's bind-pose vertices
//        (p' = sum_i w_i * jointMatrix[idx_i] * p, normal with the upper 3x3)
//        and re-uploads them via IRenderDevice::updateMesh.
//
// SCOPE: CPU skinning for a HANDFUL of characters. It deliberately does NOT touch
// the GPU-driven multidraw/bindless path or add a second pipeline — it just
// rewrites each animated mesh's vertex buffer. Unskinned models / models with no
// animation are left untouched (the caller keeps drawing them statically).

#include "engine/asset/IModelLoader.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::anim {

// A reusable per-character skinning state. Construct once over a loaded Model
// (cheap: it caches sizes + scratch buffers), then call update() each frame with
// the chosen clip + time. Holds no GPU resources of its own — it drives the
// device's existing meshes through updateMesh.
class Skinner {
public:
    // Bind to a model. Returns true if the model is actually skinnable (has a skin
    // with joints, at least one animation clip, and at least one skinned primitive
    // whose mesh was uploaded to a real device). If false, the caller should keep
    // drawing the model statically (no regression). Safe to call with a model that
    // has no skin/anim — it just reports false.
    bool bind(const x3::asset::Model& model);

    bool valid() const { return m_valid; }

    // Number of clips + a clip's name/duration (for selecting idle vs walk and for
    // logging). clipIndex is clamped/ignored if out of range.
    uint32_t   clipCount() const { return (uint32_t)m_clipDurations.size(); }
    float      clipDuration(uint32_t clip) const;
    std::string_view clipName(uint32_t clip) const;

    // Find a clip whose (lower-cased) name contains ANY of the given substrings;
    // returns its index or -1 if none match. Used to locate an "idle" / "walk"
    // clip by fuzzy name (clip names vary by exporter). Order of `keys` is a
    // priority list (first match wins).
    int findClip(std::initializer_list<const char*> keys) const;

    // Advance + apply: sample `clip` at `timeSec` (looped over the clip duration),
    // recompute the joint palette, CPU-skin every skinned primitive, and re-upload
    // each via device.updateMesh. No-op if !valid(). `timeSec` may grow unbounded
    // (it is wrapped internally). Cheap enough for a few characters at 60 Hz.
    void apply(const x3::asset::Model& model, x3::rhi::IRenderDevice& device,
               uint32_t clip, float timeSec);

    // Diagnostic for the self-test: compute the joint palette at a time WITHOUT
    // touching the device, copying it into `outPalette` (16 floats per joint,
    // column-major). Returns the joint count. Lets --test-anim assert the palette
    // changes between two times.
    uint32_t computePalette(const x3::asset::Model& model, uint32_t clip,
                            float timeSec, std::vector<float>& outPalette) const;

private:
    // Sample one node's local TRS from the clip at time t (bind-pose fallback),
    // composing the result into a column-major 4x4.
    void sampleNodeLocal(const x3::asset::Model& m, uint32_t clip, int node,
                         float t, float out[16]) const;

    // Build global node matrices for all nodes at time t (m_globalScratch sized to
    // node count). Resolves parents on the fly (nodes may be in any order).
    void computeGlobals(const x3::asset::Model& m, uint32_t clip, float t,
                        std::vector<float>& globals) const;

    bool                  m_valid = false;
    int                   m_skinIndex = -1;
    std::vector<float>    m_clipDurations;     // seconds, per clip
    std::vector<std::string> m_clipNames;
    // Per-clip, per-node channel lookup: for each node, the index of its T/R/S
    // channel in the clip (-1 = none). Flattened [clip][node][3].
    std::vector<int>      m_channelLut;        // size = clipCount * nodeCount * 3
    uint32_t              m_nodeCount = 0;
    // Scratch reused across apply() to avoid per-frame allocation.
    mutable std::vector<float>             m_globalScratch;  // nodeCount * 16
    mutable std::vector<float>             m_palette;        // jointCount * 16
    mutable std::vector<x3::rhi::MeshVertex> m_vertScratch;  // per-primitive
};

// Headless self-test (--test-anim): synthesize a tiny rigged GLB (1 bone bending
// over time), load it, and assert (a) the loader reports a skin + a clip, (b) the
// joint palette differs between t=0 and t=mid, and (c) a known skinned vertex
// actually moves. Returns true iff all checks pass. No window / Vulkan.
bool runAnimSelfTest();

} // namespace x3::anim
