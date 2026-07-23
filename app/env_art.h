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

    // ---- DISTRICT API: a whole designer layout in ONE system ----
    // beginFromDir mounts a GLB library dir + creates the loader (call once);
    // addGlbInstance then loads (cached by path — repeated prefabs share one
    // upload) and places one prefab at a world transform. Used by the district
    // loader to replicate a Unity demo scene's exact placements.
    bool beginFromDir(x3::rhi::IRenderDevice& device, std::string_view glbDir);
    bool addGlbInstance(std::string_view relPath, const float transform[16]);

    // Skip primitives whose glTF NODE NAME *or* MATERIAL NAME contains any of
    // `subs` (lowercased substring match, mirrors namedBounds()'s mechanics) —
    // applied at drawable-creation time in loadAsset(), so a matching primitive
    // never gets a ModelDrawable and never draws. Node-name matching handles
    // multi-node models (a whole node's mesh is dropped); material-name matching
    // covers baked SINGLE-node scenes (e.g. a merged flora bake) where per-part
    // identity lives in the material, not the node. Must be called BEFORE the
    // load call (build/buildFromGlb/beginFromDir+addGlbInstance) whose assets it
    // should filter — it only affects assets loaded afterward. Default empty =
    // zero behavior change.
    void setNodeSkip(std::vector<std::string> subs);

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

    // FOLIAGE: mark this system's meshes as vegetation so the PBR shader wraps the
    // diffuse + adds warm back-translucency (sun glowing through a canopy). Applies
    // to every instance/primitive this system draws. 0 = off (default; unchanged).
    void setFoliage(float f) { m_foliage = f; }

    // METALLIC CLAMP (the engine's BLACK-PROP fix, see drawMeshPBR): kit packs whose
    // packed MR maps bake metallic~1 render black (no diffuse lobe + dark env spec).
    // Clamp metallic to `m` (e.g. 0.2) so the albedo actually lights. 1 = off.
    void setMetallicClamp(float m) { m_metalClamp = m; }

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
    float                                    m_foliage = 0.0f; // >0 = vegetation shading
    float                                    m_metalClamp = 1.0f; // <1 = BLACK-PROP fix
    std::vector<std::string>                 m_nodeSkip; // lowercased node/material-name skip substrings
};

} // namespace x3::game
