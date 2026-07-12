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
    bool mounted() const { return !m_root.empty(); }
    const SurfaceSet& get(x3::rhi::IRenderDevice& device, const std::string& name);

    // ---- W8-3 streaming support (the WorldStreamer's SHARED library). A
    // streamer-lifetime library means textures are decoded ONCE per process, not
    // per region realize (a city rebuild was a 2 s PNG-decode hitch), and the
    // region ledger must EXCLUDE shared textures from per-region teardown:
    // ownsTexture answers that; destroyAll releases everything at streamer
    // shutdown (invalid handles skipped).
    bool ownsTexture(uint32_t id) const;
    void destroyAll(x3::rhi::IRenderDevice& device);

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

// BOOT-TIME: kick parallel background PNG decodes for the given sets (each set =
// albedo/normal/mr under <rootDir>/<name>/). Pure CPU, safe pre-device — the same
// overlap idea as prewarmModelDecodesAsync. SurfaceLibrary::get() consumes the
// decoded pixels (createTexture only) instead of stbi-decoding on the main thread;
// sets never prewarmed keep the original inline path. Decoded pixels for a name
// are freed once consumed. (The canonlevel recipe pass was 2.5 s of serial main-
// thread PNG decode inside the world build — the boot-regression hunt, task #4.)
void prewarmSurfaceSetsAsync(const std::string& rootDir,
                             const std::vector<std::string>& names);

// One-off PNG -> RGBA8 pixel decode (empty vector on any failure — missing
// file, LFS pointer stub, corrupt data). Exposed for texture consumers that
// need the PIXELS rather than a device handle (the rifthub membrane-flipbook
// atlas slices itself into per-frame tiles before upload). Same file-local
// stb_image instance as the set loader.
std::vector<uint8_t> decodePngRGBA8(const std::string& path, int& outW, int& outH);

// --screenshot-matlib: headless preview host. Builds one wall+floor bay per
// curated set under a neutral warm-key/cool-fill rig and captures a closeup per
// set plus two wide overview rows into `outDir` (FLAT folder for review).
// Returns a process exit code.
int runMatlibShot(x3::rhi::IRenderDevice& device, const std::string& outDir);

} // namespace x3::game
