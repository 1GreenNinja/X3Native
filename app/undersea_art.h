#pragma once
// EFLZ Act-4 undersea-base art overlay (EFLZ art pass). Mirrors env_art.{h,cpp}.
//
// Game/slice code only — engine/ stays pure. Loads the converted "Abyssal
// Station" GLB (kitbashed from the licensed Sci-Fi Space Stations Creator pack:
// a 6-leg seabed hub + riser + lit habitat deck + octagonal sub-dock + life-
// support/quarters pods, assembled in Blender) and draws it as a STATIC visual
// prop OVER the OceanBase graybox (ocean_base.cpp keeps the graybox disc as the
// truth; this only adds the real textured art on top). Decoupling visual art
// from the graybox is the same safe path env_art uses for Level 1.
//
// Pattern mirrors EnvArtSystem: own the IAssetSource + loader + Model so the GPU
// handles stay valid for the app lifetime, build node-TRS-baked ModelDrawables,
// and issue drawMesh calls each frame at the authored instance transform. The
// converted GLB carries PBR base/normal/MR maps; its glTF emissive *textures* are
// not sampled by drawMeshPBR (same as env_art), so the lit-deep-sea read comes
// from (a) a subtle per-instance emissive on the station + (b) cool POINT LIGHTS
// registered around it (the env_art Light_A pattern) feeding the bloom chain.
//
// FALLBACK: if the GLB fails to load, nothing is drawn and the OceanBase graybox
// stays visible — the zone never breaks.
//
// CLEAN-ROOM: built from the IModelLoader + IAssetSource + IRenderDevice + Scene
// interfaces + the OceanBasePlan only. No third-party engine source consulted.

#include "scene.h"
#include "ocean_base.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// One loaded undersea asset: Model + node-TRS-baked drawables. `ok` false if the
// GLB failed to load (its instances are skipped).
struct UnderseaAsset {
    x3::asset::Model                      model;
    std::vector<x3::asset::ModelDrawable> drawables;
    bool ok = false;
};

// One placed instance: index into the asset table + world transform (column-major
// 4x4). emissive = { r, g, b, strength } (linear); strength > 1 makes it an HDR
// bloom source. Default {0,0,0,0} = no glow.
struct UnderseaInstance {
    uint32_t asset = 0;
    float    transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float    emissive[4]   = {0,0,0,0};
};

class UnderseaArtSystem {
public:
    // Load the converted Abyssal Station GLB from `convertedGlbDir` (the loose
    // converted_glb root) and place it over the OceanBase graybox described by
    // `plan` (seated on the base top deck at the disc center). Registers cool
    // point-light fixtures around the station so the PBR hull reads as lit in the
    // deep + feeds bloom. Safe to call once after OceanBase::build(). If the GLB
    // is missing it loads nothing (the graybox stays); query any().
    void build(x3::rhi::IRenderDevice& device,
               std::string_view convertedGlbDir,
               const OceanBasePlan& plan);

    // Draw all placed instances (static; call alongside scene.render() each frame).
    // No-op if nothing loaded.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Diagnostics for logging / the self-test.
    uint32_t assetsLoaded() const;
    uint32_t instanceCount() const { return (uint32_t)m_instances.size(); }
    bool any() const { return assetsLoaded() > 0; }

    // Cool point lights placed around the station (one per fixture). Feed these to
    // IRenderDevice::setPointLights so the undersea base reads as a lit structure.
    const std::vector<x3::rhi::PointLight>& lightFixtures() const { return m_lightFixtures; }

private:
    uint32_t loadAsset(const std::string& relPath);
    void addInstanceEmissive(uint32_t a, const float transform[16], const float emissive[4]);

    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<UnderseaAsset>               m_assetTable;
    std::vector<std::string>                 m_assetPaths; // parallel to m_assetTable
    std::vector<UnderseaInstance>            m_instances;
    std::vector<x3::rhi::PointLight>         m_lightFixtures;
};

// Headless self-test (--test-undersea-art). Builds the OceanBase graybox + the
// undersea art overlay on a HeadlessDevice and asserts: the station GLB loads, at
// least one instance is placed, and cool point-light fixtures are registered.
// Prints "undersea-art: X/Y passed"; returns true iff all pass. No window/Vulkan.
bool runUnderseaArtSelfTest();

} // namespace x3::game
