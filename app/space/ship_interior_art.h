#pragma once
// ship_interior_art — the REAL ship-interior art (integration feast, Tim's
// order: "Make the real ship interior, walkable interactable... we have all
// the assets, we should use them").
//
// The env_art OVERLAY pattern: the S5 graybox (ship_interior.*) stays as the
// collision + per-piece fallback; this module drapes the LICENSED 3D Scifi Kit
// Vol 3 pieces (converted with embedded PBR textures to assets/converted_glb/
// SciFiKit3/) over the manifest as pure Scene render entities — kit cladding
// walls set slightly inside the graybox shells, real deck tiles, a doorframe
// at the room join, crew fixtures (bunk, lockers, galley, computer wall,
// consoles, chair, screens, props). Placements are derived from the pieces'
// MEASURED GLB AABBs (see the fold commit) against makeSmallCockpit's dims.
//
// A missing GLB just leaves the graybox visible there (the level never
// breaks). The S6 window portals draw INSIDE the cladding offset, so the
// moving-space view stays visible; the forward window wall is deliberately
// left unclad (the portal dominates that wall).
//
// INTERACT: stations() exposes the console art entities + world positions so
// the host can run E-prompts against them (host_shipwindows owns the input).

#include "ship_interior.h"
#include "../scene.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"

#include <memory>
#include <string>
#include <vector>

namespace x3::space {

class ShipInteriorArt {
public:
    // One interactable station fixture the art placed (console + its screen).
    struct ArtStation {
        std::string kind;          // "helm" / "nav" (from the manifest)
        float       pos[3];        // world floor position (prompt anchor)
        uint32_t    screenEntity;  // Scene id of the console screen (emissive pulse target)
    };

    // Load + place the kit over `manifest`. Safe to call with assets missing
    // (logs + falls back per piece). Returns the number of art entities placed.
    uint32_t build(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
                   const ShipManifest& manifest);

    const std::vector<ArtStation>& stations() const { return m_stations; }
    uint32_t entityCount() const { return m_entities; }

    void shutdown();   // unloads models (Scene owns nothing GPU-side here)

private:
    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<x3::asset::Model>            m_models;   // one per loaded piece
    std::vector<ArtStation>                  m_stations;
    x3::rhi::TextureHandle                   m_mrShared; // 1x1 MR: forces PBR route (emissiveTex honor)
    x3::rhi::IRenderDevice*                  m_device = nullptr;
    uint32_t                                 m_entities = 0;
};

} // namespace x3::space
