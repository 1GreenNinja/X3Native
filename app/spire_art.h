#pragma once
// EFLZ Spire per-floor THEMED art overlay (spire_art pass).
//
// Sibling of env_art.h, which paints the base ModularSciFi look uniformly
// across the whole Spire. spire_art layers PER-FLOOR THEMED dressing ON TOP of
// that base — e.g. the "Modular Abandoned Hospital" kit on F3 Genetics Lab for
// clinical-horror atmosphere — driven by mined-from-Unity placement manifests
// under `tools/manifests/<floor>_*.x3lvl.json` (see tools/mine_unity_scene.py).
//
// Same operational contract as env_art:
//   * Purely VISUAL (no physics, no gameplay). The graybox boxes from level1.cpp
//     remain the truth for collision/queries; level1 + env_art floors/walls/
//     ceilings still render where this overlay does NOT cover them.
//   * Per-asset FALLBACK: a missing GLB is silently not-drawn; the level never
//     breaks — worst case the floor falls back to the env_art base look.
//   * Per-instance EMISSIVE (HDR pipeline): drive surgical lamps / signage hard
//     so they feed the bloom chain (this is THE clinical-horror look on F3).
//   * Per-floor SUPPRESSION mask: returned by build() so env_art can suppress
//     its base floor/wall/ceiling render on covered floors (avoids z-fighting
//     and lets the hospital walls actually read as the dominant surface).
//
// Pattern mirrors env_art.h, weapon_system, monster_system: own the
// IAssetSource + loader + Models so the GPU handles stay valid for the app
// lifetime; build per-primitive ModelDrawables; issue drawMesh calls per frame
// at the authored instance transforms.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice + Scene
// interfaces only. No purchased C# / id Tech / RBDOOM source consulted.

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

// ---- One loaded themed-art asset (a converted GLB) + its drawables. ----------
// `ok` is false if the GLB failed to load; instances of it are silently skipped
// in draw() so the floor's env_art base stays visible there.
struct SpireArtAsset {
    x3::asset::Model                      model;
    std::vector<x3::asset::ModelDrawable> drawables;
    bool ok = false;
};

// ---- One placed instance from a floor's themed manifest. ---------------------
// `floor` lets draw()/diagnostics filter by floor (e.g. cull-by-floor someday,
// or report per-floor stats). `emissive` is the HDR per-instance term: rgb is
// linear color, w is strength. Default {0,0,0,0} means "no glow"; surgical-lamp
// fixtures set it > 1 so the panel reads as a bright HDR source feeding bloom.
struct SpireArtInstance {
    uint32_t asset = 0;
    float    transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float    emissive[4]   = {0,0,0,0};
    L1Floor  floor = (L1Floor)0;
};

// ---- Per-floor coverage report. ----------------------------------------------
// build() fills floorCovered[F3] = true when F3's hospital overlay loaded any
// real GLBs and the host can choose to suppress env_art's base floor/wall/
// ceiling render on those floors. (env_art will continue rendering on the
// non-covered floors as before.)
struct SpireArtMask {
    bool floorCovered[(uint32_t)L1Floor::Count] = {};
};

// ---- The system. -------------------------------------------------------------
// Owns the asset source + loader + asset table + instance list + the per-floor
// point-light fixtures registered for forward+ lighting. Lifecycle:
//   1. build() once before the first frame -> mounts `convertedGlbDir`, loads
//      every per-floor manifest under `manifestsDir`, anchors instances at the
//      floor's base Y from `layout`/level1Rooms().
//   2. draw() each frame, after env_art::draw(), so themed art layers on top.
//   3. lightFixtures() exposed for the host to merge with env_art's lights and
//      submit to IRenderDevice::setPointLights once per frame.
class SpireArtSystem {
public:
    // Mount + load all per-floor manifests; return per-floor coverage mask.
    // `convertedGlbDir`: where the converted hospital/other pack GLBs live (e.g.
    //   "<repo>/assets/converted_glb"). The manifests reference packs relative
    //   to this dir, so the same mount handles every floor's pack(s).
    // `manifestsDir`: directory containing per-floor manifest JSON files named
    //   "<floor>_*.x3lvl.json" (e.g. "f3_overview.x3lvl.json"). build() scans
    //   the dir and loads each.
    // `layout`: passed through to anchor instances at each floor's base Y via
    //   level1Rooms()[floor].y0 (matches the Spire's non-uniform stack:
    //   B1=0 / F1=5 / F2=10 / F3=20 / F4=30 / F5=65 / F6=78 / F7=91 meters).
    // Missing dir / missing manifests / failing GLBs degrade gracefully (the
    // mask remains all-false for those floors; env_art's base stays).
    SpireArtMask build(x3::rhi::IRenderDevice& device,
                       std::string_view convertedGlbDir,
                       std::string_view manifestsDir,
                       const Level1Layout& layout);

    // Draw all placed instances. Call AFTER env_art::draw() so themed dressing
    // layers on top. No-op if nothing loaded.
    void draw(x3::rhi::IRenderDevice& device,
              const x3::rhi::FrameContext& frame) const;

    // Forward point lights captured at build time (e.g. for surgical-lamp
    // fixtures on F3). Empty if no light-emitting fixtures placed. The host
    // merges these with env_art::lightFixtures() and submits the union.
    const std::vector<x3::rhi::PointLight>& lightFixtures() const {
        return m_lightFixtures;
    }

    // Diagnostics for logging / host stats.
    uint32_t assetsLoaded() const;
    uint32_t instanceCount() const { return (uint32_t)m_instances.size(); }
    uint32_t instanceCountOnFloor(L1Floor f) const;
    bool any() const { return assetsLoaded() > 0; }

private:
    // Load one GLB by relative path under the mounted converted_glb dir; cached
    // by path so multi-use kit pieces share one upload. Returns the asset index
    // (always valid — a failed load yields ok=false and draws nothing).
    uint32_t loadAsset(const std::string& relPath);

    void addInstance(uint32_t a, L1Floor f, const float transform[16]);
    void addInstanceEmissive(uint32_t a, L1Floor f,
                             const float transform[16],
                             const float emissive[4]);

    // Parse one manifest JSON for a single floor, anchor each placement at
    // `layout.<floor>.y0`, push instances. Returns true if at least one GLB
    // loaded ok and at least one instance was placed.
    bool loadManifestForFloor(L1Floor floor,
                              std::string_view manifestPath,
                              std::string_view glbSubdir,
                              const Level1Layout& layout);

    // Map a manifest filename (e.g. "f3_overview.x3lvl.json") to its L1Floor.
    // Returns true on match. Unknown floor names are skipped with a warn log.
    static bool floorFromManifestName(std::string_view fileName, L1Floor& out);

    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<SpireArtAsset>               m_assetTable;
    std::vector<std::string>                 m_assetPaths;     // parallel
    std::vector<SpireArtInstance>            m_instances;
    std::vector<x3::rhi::PointLight>         m_lightFixtures;
};

} // namespace x3::game
