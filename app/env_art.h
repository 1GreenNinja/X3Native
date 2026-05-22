#pragma once
// EFLZ environment art overlay (EFLZ art pass).
//
// Game/slice code only — engine/ stays pure. Loads the converted sci-fi GLBs
// (ModularSciFi Interior kit) and draws them as STATIC visual props OVER the
// Level 1 graybox collision (level1.cpp keeps the Jolt static bodies; this only
// adds visuals). Decoupling visual art from collision is the safest path: the
// graybox boxes stay as the true collision, and the GLB walls/floor/ceiling/door
// frames/console are drawn at matching transforms so the room reads as a real
// sci-fi corridor instead of a checkerboard graybox.
//
// Pattern mirrors WeaponSystem/MonsterSystem: own the IAssetSource + loader +
// Models so the GPU handles stay valid for the app lifetime, build per-primitive
// ModelDrawables (now node-TRS-baked, M2 fix), and issue the drawMesh calls each
// frame at authored instance transforms. PURELY VISUAL — no physics, no gameplay.
//
// Per-asset FALLBACK: if a GLB fails to load, that asset's instances are simply
// not drawn; level1.cpp's graybox surface render stays visible for that surface
// (the EnvArtSystem reports which surfaces loaded via a Level1ArtMask so the host
// can suppress the graybox render only where real art replaces it). The level
// never breaks: worst case it falls back to the original graybox look.

#include "scene.h"
#include "level1.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// One loaded environment asset: its Model + per-primitive drawables (node-TRS
// baked). `ok` is false if the GLB failed to load (instances of it are skipped).
struct EnvAsset {
    x3::asset::Model                      model;
    std::vector<x3::asset::ModelDrawable> drawables;
    bool ok = false;
};

// One placed instance: an index into the asset table + its world transform
// (column-major 4x4). Drawn as objectTransform * nodeTransform per drawable.
// `emissive` (HDR pipeline) is the per-instance emissive radiance: rgb = linear
// color, w = strength. Default {0,0,0,0} = no glow; the Light_A ceiling fixtures
// set it >0 so they read as bright HDR sources that drive the bloom chain.
struct EnvInstance {
    uint32_t asset = 0;        // index into m_assets
    float    transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float    emissive[4] = {0,0,0,0}; // rgb = linear color, w = strength
};

class EnvArtSystem {
public:
    // Load the converted environment GLBs from `convertedGlbDir` (the loose dir
    // root, e.g. "G:/GameModels/converted_glb") and place static instances that
    // cover the Level 1 rooms described by `layout`. Returns a Level1ArtMask
    // telling the caller which graybox surfaces are now covered by real art (so it
    // can suppress those graybox renders). Safe to call once before buildLevel1.
    // If the dir/GLBs are missing, returns an all-false mask (full graybox kept).
    Level1ArtMask build(x3::rhi::IRenderDevice& device,
                        std::string_view convertedGlbDir,
                        const Level1Layout& layout);

    // Draw all placed environment instances (static; call alongside scene.render()
    // each frame, before the viewmodel). No-op if nothing loaded.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Diagnostics for logging / the host: how many assets loaded ok / instances.
    uint32_t assetsLoaded() const;
    uint32_t instanceCount() const { return (uint32_t)m_instances.size(); }
    bool any() const { return assetsLoaded() > 0; }

    // Forward point lights for the Light_A ceiling fixtures placed by build(): one
    // warm-white omni at each fixture's world position (the meshes only model the
    // fixture; this is where the light should emit). Captured at build time; empty
    // if the light kit piece failed to load. Feed these to
    // IRenderDevice::setPointLights so the corridor reads as a lit interior.
    const std::vector<x3::rhi::PointLight>& lightFixtures() const { return m_lightFixtures; }

private:
    // Load one GLB by relative path under the mounted dir; returns the asset index
    // (always valid — a failed load yields an EnvAsset with ok=false that draws
    // nothing). Cached by path so repeated kit pieces share one upload. Uses the
    // loader created in build() (already bound to the render device).
    uint32_t loadAsset(const std::string& relPath);

    // Add an instance of asset `a` at the given world transform.
    void addInstance(uint32_t a, const float transform[16]);
    // Add an instance with a per-instance EMISSIVE term (HDR pipeline): emissive =
    // { r, g, b, strength } in linear light. Used for the Light_A fixtures so they
    // glow as bright HDR sources (feeding bloom). emissive == nullptr -> no glow.
    void addInstanceEmissive(uint32_t a, const float transform[16], const float emissive[4]);

    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<EnvAsset>                    m_assetTable;
    std::vector<std::string>                 m_assetPaths; // parallel to m_assetTable
    std::vector<EnvInstance>                 m_instances;
    std::vector<x3::rhi::PointLight>         m_lightFixtures; // omni per Light_A fixture
};

} // namespace x3::game
