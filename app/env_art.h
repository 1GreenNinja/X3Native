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
    std::vector<std::string>              drawableNames;  // glTF node name per drawable (parallel)
    bool ok = false;
};

// One SCATTERED clone of a single drawable (foliage densification). Unlike an
// EnvInstance (which re-draws the WHOLE asset), a ScatterDraw re-draws ONE
// drawable at a full world transform of its own — so a forest can be thickened
// with per-tree position/yaw/scale variation instead of a rigid group copy.
struct ScatterDraw {
    uint32_t asset    = 0;
    uint32_t drawable = 0;
    float    transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};  // world (replaces nodeTransform)
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

    // Load ONE arbitrary converted GLB (e.g. a baked Unity scene export) and place a
    // single identity instance, so draw() renders ALL its primitives at their baked
    // node transforms. Self-contained (creates its own IAssetSource + loader). Used by
    // the --screenshot-showroom / --world showroom scene-preview path. Returns true if
    // the GLB loaded (false keeps an empty system — nothing draws).
    bool buildFromGlb(x3::rhi::IRenderDevice& device,
                      std::string_view convertedGlbDir, std::string_view relPath);

    // Same, but place the single instance at `transform` (column-major 4x4)
    // instead of identity — e.g. the hero car posed on the showroom floor
    // (--screenshot-car). nullptr = identity (exactly buildFromGlb above).
    bool buildFromGlbAt(x3::rhi::IRenderDevice& device,
                        std::string_view convertedGlbDir, std::string_view relPath,
                        const float transform[16]);

    // Replace instance `idx`'s transform (column-major). For the turntable
    // capture rig (re-pose the car between stills) — cheap, no reload.
    void setInstanceTransform(uint32_t idx, const float transform[16]);

    // World-space AABB of all placed drawables' origins (engine-space ground truth —
    // for framing a preview camera). outMin/outMax are float[3]. No-op (huge/inverted)
    // if nothing is placed.
    void worldBounds(float outMin[3], float outMax[3]) const;

    // Engine-space AABB of mesh-node origins whose node NAME contains any of `subs`
    // (lowercased substring match) — for framing the camera on a SUBSET (e.g. the
    // building, ignoring far decorative scatter). Uses the SAME node-transform
    // composition as makeDrawables, so it matches what's rendered. Returns the match
    // count; the bbox is inverted (min>max) if nothing matched.
    uint32_t namedBounds(const std::vector<std::string>& subs, float outMin[3], float outMax[3]) const;

    // Draw all placed environment instances (static; call alongside scene.render()
    // each frame, before the viewmodel). No-op if nothing loaded. `maxDrawables` caps
    // how many per-primitive drawables are issued this frame (default: all) — used by
    // the showroom diagnostic to bisect a per-frame draw-count fault.
    // Returns the number of drawables actually issued (after cull/cap) — for diagnostics.
    // If cullMin/cullMax are non-null, only drawables whose world origin is inside that AABB
    // are issued (frames a region of a large baked scene + bounds the per-frame draw count).
    uint32_t draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  uint32_t maxDrawables = 0xFFFFFFFFu,
                  const float* cullMin = nullptr, const float* cullMax = nullptr) const;

    // FOLIAGE DENSIFY (showroom forest). Thickens an already-placed baked scene's
    // foliage by CLONING its matching drawables (node name contains any of `nameSubs`,
    // lowercased substring match) into extra scatter draws. Each clone is seeded from a
    // random EXISTING one — so it inherits a position the artist already put ON the
    // ground — then offset in XZ by [minR,maxR] metres, re-yawed, and re-scaled in
    // [scaleMin,scaleMax]. It is sunk by `sink` metres so a clone that lands on a slope
    // buries its trunk instead of floating (a hovering tree is fatal; a slightly buried
    // one is invisible under the snow skirt). Clustering around existing trees is also
    // what a real forest does — it breaks the lattice instead of building one.
    // Returns the number of clones added. No-op if nothing matches.
    // `keepOutXZR` (nullable) = { centerX, centerZ, radius }: clones landing inside that
    // XZ disc are rejected, so the densified forest never grows onto the hero building's
    // apron/platform.
    uint32_t densifyFoliage(const std::vector<std::string>& nameSubs, uint32_t addCount,
                            uint32_t seed, float minR, float maxR,
                            float scaleMin, float scaleMax, float sink,
                            const float* keepOutXZR = nullptr);

    // Diagnostics for logging / the host: how many assets loaded ok / instances.
    uint32_t scatterCount() const { return (uint32_t)m_scatter.size(); }
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
    std::vector<ScatterDraw>                 m_scatter;   // foliage densify clones (drawn after m_instances)
    std::vector<x3::rhi::PointLight>         m_lightFixtures; // omni per Light_A fixture
};

} // namespace x3::game
