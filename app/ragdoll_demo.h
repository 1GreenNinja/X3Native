#pragma once
// Ragdoll demo + app-side blend self-test (Physics §2) — `--world ragdoll` /
// the Skinner-blend half of `--test-ragdoll`. Game/slice code only; engine/ stays
// pure.
//
// `--world ragdoll`: a lit ground + a standing humanoid built from the canonical
// 11-bone ragdoll rig. It starts as a fixed (animated-pose stand-in) figure; press
// R (or after a timed trigger in headless capture) it RAGDOLLS — the Jolt ragdoll
// takes over and the figure collapses naturally under gravity. Press T to blend
// BACK toward the stand pose (partial -> full). Each bone is drawn as a scaled cube
// at its physics world transform, so the collapse is visible without a skinned GLB.
//
// The headless `runRagdollBlendCheck()` builds a SYNTHETIC skinned model whose node
// names match the ragdoll bone names, drives anim::Skinner::computeRagdollBlendedPalette
// across blend weight 0->1, and asserts the joint palette interpolates MONOTONICALLY
// from the animated pose to the ragdoll pose (the §2 "blend weight 0->1 interpolates
// the palette monotonically" acceptance check, exercised through the real Skinner).
//
// Built ONLY through the public IRenderDevice + IPhysicsWorld + IRagdoll + Skinner
// interfaces. Jolt (MIT) is the only physics lib. No Unreal / id Tech / RBDOOM or
// any other game-engine source consulted.

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Ragdoll.h"
#include "engine/asset/IModelLoader.h"
#include "anim.h"
#include "mesh_prims.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// `--world ragdoll` render demo.
// ---------------------------------------------------------------------------
class RagdollDemo {
public:
    void build(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics) {
        m_device  = &device;
        m_physics = &physics;

        // Shared cube mesh (half-extent 0.5) + textures.
        std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
        x3::prims::makeCube(0.5f, cv, ci);
        m_cube = device.createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
        auto boneTex = x3::prims::makeCheckerRGBA(64, 8, 210, 170, 120, 150, 110, 70);
        m_boneTex = device.createTexture(boneTex.data(), 64, 64, true);
        auto groundTex = x3::prims::makeCheckerRGBA(64, 8, 150, 150, 160, 60, 62, 74);
        m_groundTex = device.createTexture(groundTex.data(), 64, 64, true);

        // Ground (static collision + render quad), top at y=0.
        x3::prims::PrimMesh g = x3::prims::makeBox(12.0f, 0.25f, 12.0f, 0.0f, -0.25f, 0.0f, 0.25f);
        m_groundMesh = device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                         g.index.data(), (uint32_t)g.index.size());
        physics.addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size() / 3),
                              g.cindex.data(), (uint32_t)g.cindex.size());

        // Build the canonical humanoid ragdoll rig (pelvis ~1.1 m up so it stands).
        x3::phys::makeHumanoidRagdollBones(1.1f, m_bones);
        m_ragdoll.reset(x3::phys::createRagdoll(physics, m_bones.data(),
                                                (uint32_t)m_bones.size()));
        if (m_ragdoll) {
            // Add to the world but keep it kinematic-ish by NOT activating until the
            // ragdoll trigger fires; we show the bind pose until then.
            m_ragdoll->addToWorld(/*activate*/false);
            // Snap to the bind/stand pose.
            std::vector<float> bind(m_bones.size() * 16);
            for (size_t i = 0; i < m_bones.size(); ++i)
                std::memcpy(&bind[i*16], m_bones[i].bindWorld, 16 * sizeof(float));
            m_ragdoll->setPoseWorld(bind.data());
        }
        physics.optimizeBroadphase();
    }

    // Trigger the ragdoll: activate the bodies + give a small topple impulse.
    void ragdollize() {
        if (!m_ragdoll || m_active) return;
        m_ragdoll->addToWorld(true);   // idempotent if already added (it is)
        m_ragdoll->applyImpulseAll(x3::phys::Vec3{ 1.0f, 0.5f, 0.4f });
        m_active = true;
    }

    bool active() const { return m_active; }

    void render(const x3::rhi::FrameContext& frame) const {
        if (!m_device) return;
        const float white[4] = { 1, 1, 1, 1 };
        const float idG[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        m_device->drawMesh(frame, m_groundMesh, m_groundTex, white, idG);

        if (!m_ragdoll) return;
        std::vector<float> w(m_bones.size() * 16);
        m_ragdoll->getBoneWorldTransforms(w.data());
        const float boneCol[4] = { 0.9f, 0.8f, 0.7f, 1.0f };
        for (size_t i = 0; i < m_bones.size(); ++i) {
            // Draw a thin box along the bone's +Y (the capsule axis), length ~2*hh.
            float m[16];
            std::memcpy(m, &w[i*16], sizeof(m));
            const float sx = m_bones[i].radius * 2.0f / 0.5f;          // x scale
            const float sy = (m_bones[i].halfHeight * 2.0f) / 0.5f;    // y scale (along bone)
            const float sz = m_bones[i].radius * 2.0f / 0.5f;          // z scale
            // The body origin is at the bone base; shift the drawn cube up by hh so
            // it spans base..tip (the capsule was offset by +hh in local space).
            float ty = m_bones[i].halfHeight;
            // Apply local +Y offset (ty) in the bone frame: translate along col1.
            m[12] += m[4]*ty; m[13] += m[5]*ty; m[14] += m[6]*ty;
            m[0]*=sx; m[1]*=sx; m[2]*=sx;
            m[4]*=sy; m[5]*=sy; m[6]*=sy;
            m[8]*=sz; m[9]*=sz; m[10]*=sz;
            m_device->drawMesh(frame, m_cube, m_boneTex, boneCol, m);
        }
    }

    void shutdown() {
        if (m_ragdoll) { m_ragdoll->removeFromWorld(); m_ragdoll.reset(); }
        if (m_device) {
            if (m_cube.valid())       m_device->destroyMesh(m_cube);
            if (m_groundMesh.valid()) m_device->destroyMesh(m_groundMesh);
            if (m_boneTex.valid())    m_device->destroyTexture(m_boneTex);
            if (m_groundTex.valid())  m_device->destroyTexture(m_groundTex);
        }
        m_device = nullptr; m_physics = nullptr;
    }

    x3::phys::IRagdoll* ragdoll() { return m_ragdoll.get(); }

private:
    x3::rhi::IRenderDevice*   m_device  = nullptr;
    x3::phys::IPhysicsWorld*  m_physics = nullptr;
    std::unique_ptr<x3::phys::IRagdoll> m_ragdoll;
    std::vector<x3::phys::RagdollBoneDesc> m_bones;
    bool m_active = false;

    x3::rhi::MeshHandle    m_cube;
    x3::rhi::MeshHandle    m_groundMesh;
    x3::rhi::TextureHandle m_boneTex;
    x3::rhi::TextureHandle m_groundTex;
};

// ---------------------------------------------------------------------------
// App-side blend self-test (the Skinner half of --test-ragdoll). Builds a tiny
// synthetic skinned model whose node names match the ragdoll bone names, then
// drives the REAL anim::Skinner ragdoll-blend across weight 0->1 and asserts the
// joint palette interpolates monotonically from the animated pose toward the
// ragdoll pose. Prints PASS/FAIL lines; returns the (pass,total) counts via refs.
// No window / Vulkan (headless; palette-only). Returns true iff all checks pass.
// ---------------------------------------------------------------------------
inline bool runRagdollBlendCheck(int& outPass, int& outTotal) {
    int pass = 0, total = 0;
    auto P = [&](bool cond, const char* name) {
        ++total;
        if (cond) { ++pass; x3::logInfo(std::string("[ragdoll-blend] PASS ") + name); }
        else      {          x3::logError(std::string("[ragdoll-blend] FAIL ") + name); }
    };

    // 1) Build a minimal skinned model: a 3-bone vertical chain (b0->b1->b2). Node
    //    names match three ragdoll bone names so resolveExternalBones() maps them.
    x3::asset::Model model;
    const char* names[3] = { "pelvis", "spine", "head" };
    // Nodes: a simple chain along +Y, each 0.4 m above the previous (local TRS).
    for (int i = 0; i < 3; ++i) {
        x3::asset::Node nd;
        nd.parent = (i == 0) ? -1 : (i - 1);
        nd.name = names[i];
        // local translation: root at origin, children +0.4 in Y relative to parent.
        float ty = (i == 0) ? 0.0f : 0.4f;
        float lt[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,ty,0,1};
        std::memcpy(nd.localTransform, lt, sizeof(lt));
        nd.skinIndex = 0;
        model.nodes.push_back(nd);
    }
    // Skin: 3 joints, identity inverse-bind (bind pose == authored pose here, so the
    // palette at weight 0 with the SAME animated pose is the bind global * I).
    x3::asset::Skin skin;
    skin.joints = { 0, 1, 2 };
    skin.inverseBind.assign(3 * 16, 0.0f);
    for (int j = 0; j < 3; ++j) {
        // inverse-bind = inverse of the bind global. Bind globals: y = 0, 0.4, 0.8.
        float by = (j == 0) ? 0.0f : (j == 1 ? 0.4f : 0.8f);
        float ib[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-by,0,1};
        std::memcpy(&skin.inverseBind[j*16], ib, sizeof(ib));
    }
    model.skins.push_back(skin);
    // One skinned primitive (1 vertex weighted to joint 2) so bind() reports valid.
    x3::asset::MeshPrimitive prim;
    prim.skinned = true;
    prim.basePos = { 0.0f, 0.8f, 0.0f };
    prim.baseNrm = { 0.0f, 1.0f, 0.0f };
    prim.baseUv  = { 0.0f, 0.0f };
    prim.jointIdx = { 2, 0, 0, 0 };
    prim.jointWt  = { 1.0f, 0.0f, 0.0f, 0.0f };
    model.primitives.push_back(prim);
    // A trivial 1-channel clip so the model is "animated" (a single rotation key on
    // node 0). bind() requires at least one animation clip with a channel.
    x3::asset::AnimationClip clip;
    clip.name = "stand";
    clip.duration = 1.0f;
    x3::asset::AnimationChannel ch;
    ch.targetNode = 0;
    ch.path = x3::asset::AnimPath::Rotation;
    ch.components = 4;
    ch.times = { 0.0f };
    ch.values = { 0.0f, 0.0f, 0.0f, 1.0f };  // identity quat
    clip.channels.push_back(ch);
    model.animations.push_back(clip);

    x3::anim::Skinner sk;
    bool bound = sk.bind(model);
    P(bound, "B1 synthetic skinned model binds");
    if (!bound) { outPass = pass; outTotal = total; return false; }

    uint32_t resolved = sk.resolveExternalBones(model, names, 3);
    P(resolved == 3, "B2 all 3 ragdoll bone names resolve to skin joints");

    // 2) External ragdoll pose: each bone DISPLACED far from its animated pose (a
    //    big -X + -Y move) so the blend is unambiguous. World matrices, identity rot.
    float ext[3*16];
    for (int i = 0; i < 3; ++i) {
        float by = (i == 0) ? 0.0f : (i == 1 ? 0.4f : 0.8f);
        // ragdoll world: shifted -1.5 in X and dropped by an extra 1.0 in Y.
        float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0,  -1.5f, by - 1.0f, 0.0f, 1};
        std::memcpy(&ext[i*16], m, sizeof(m));
    }

    // 3) Sweep weight 0 -> 1; the skinned vertex's resulting position (joint 2's
    //    palette applied to basePos) must move MONOTONICALLY from the animated spot
    //    toward the ragdoll spot.
    auto vertX = [&](float w) -> float {
        std::vector<float> pal;
        sk.computeRagdollBlendedPalette(model, 0, 0.0f, ext, 3, w, pal);
        // joint 2 palette * basePos (0,0.8,0).
        const float* jm = &pal[2*16];
        float bp[3] = { 0.0f, 0.8f, 0.0f };
        // column-major xform.
        return jm[0]*bp[0] + jm[4]*bp[1] + jm[8]*bp[2] + jm[12];
    };
    float x0 = vertX(0.0f), x1 = vertX(1.0f);
    P(std::fabs(x0 - 0.0f) < 1e-3f, "B3 weight=0 -> animated pose (vertex at x~0)");
    P(std::fabs(x1 - (-1.5f)) < 1e-2f, "B4 weight=1 -> ragdoll pose (vertex at x~-1.5)");

    bool mono = true;
    float prev = x0;
    for (int s = 1; s <= 20; ++s) {
        float w = s / 20.0f;
        float x = vertX(w);
        if (x > prev + 1e-4f) mono = false;   // must DECREASE (toward -1.5)
        prev = x;
    }
    P(mono, "B5 blend weight 0->1 interpolates the palette monotonically");

    outPass = pass; outTotal = total;
    return pass == total;
}

} // namespace x3::game
