// SURFACE LIBRARY — real PBR texture sets from the pack library as first-class
// architectural surfaces (ART_BIBLE §4 realism mandate). Sets live under
// assets/surface_library/<name>/{albedo,normal,mr}.png (curated + channel-law
// converted by tools/tex_curate.py; mr is glTF convention G=rough B=metal,
// matching mesh.frag's metallic=mr.b). This system loads a set into device
// textures and hands out tiled panel meshes for walls/floors/ceilings so room
// recipes can dress with AUTHORED materials instead of tinted kit boxes.
#pragma once

#include "engine/rhi/IRenderDevice.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace x3::game {

struct SurfaceSet {
    x3::rhi::TextureHandle albedo{};
    x3::rhi::TextureHandle normal{};
    x3::rhi::TextureHandle mr{};
    bool ok = false;
};

class SurfaceLibrary {
public:
    // rootDir = <assetRoot>/surface_library. Loads lazily per set; caches.
    void mount(std::string rootDir) { m_root = std::move(rootDir); }
    const SurfaceSet& get(x3::rhi::IRenderDevice& device, const std::string& name);

    // A flat tiled panel: a quad of w x h meters whose UVs repeat every
    // `tileMeters`, so texel density is uniform across every surface that uses
    // the same tile size. `axis` picks the plane: 0 = wall facing -Z (XY plane),
    // 1 = floor facing +Y (XZ plane), 2 = wall facing +X (ZY plane). The mesh is
    // created fresh per call (panels are built once at level build; cache at the
    // call site if you need many identical ones).
    x3::rhi::MeshHandle makePanel(x3::rhi::IRenderDevice& device, int axis,
                                  float w, float h, float tileMeters) const;

    // Draw a loaded set on a panel mesh with neutral PBR factors.
    void drawPanel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                   const SurfaceSet& set, x3::rhi::MeshHandle mesh,
                   const float transform[16]) const;

private:
    std::string m_root;
    std::unordered_map<std::string, SurfaceSet> m_cache;
};

// --screenshot-matlib: headless preview host. Builds one wall+floor bay per
// curated set under a neutral warm-key/cool-fill rig and captures a closeup per
// set plus two wide overview rows into `outDir` (FLAT folder for review).
// Returns a process exit code.
int runMatlibShot(x3::rhi::IRenderDevice& device, const std::string& outDir);

} // namespace x3::game
