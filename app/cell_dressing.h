#pragma once
// EFLZ OPENING-SPACE polish: dense set-dressing + motivated lighting for the canon
// Floor-1 DETENTION cell + the first rooms the player sees on `--world canonlevel`.
//
// WHY THIS EXISTS: the data-driven canon floor (level_loader.cpp) builds only graybox
// room shells + one warm per-room ceiling light + the gameplay characters. The opening
// cell therefore reads as an empty blue box — "lacks substance + polish". This module
// is a PURELY VISUAL + LIGHTING overlay (no collision, no gameplay) that drapes real
// PBR props (the ModularSciFi Interior + SciFi Warehouse kits) and motivated, moody
// lights over the SAME playable space — a bunk, a wall terminal, pipes, cabling, crates,
// debris, a fusebox, a door frame, signage, plus a flickering cell light, a red alarm
// strip, and cyan terminal glow. The graybox boxes stay the collision truth; this only
// adds visuals + PointLights. Per-asset fallback: a missing GLB simply isn't drawn.
//
// Pattern mirrors EnvArtSystem (own IAssetSource + loader + Model cache; build per-prim
// ModelDrawables; issue drawMeshPBR each frame at authored transforms), but driven by the
// CanonRoom geometry instead of the legacy Level1Layout, with a flexible per-prop placer.

#include "level_loader.h"   // CanonFloor / CanonRoom / CanonBeats

#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

class CellDressing {
public:
    // Build the opening-space dressing for a parsed canon floor: dense props in Jake's
    // Cell + the Main Hall mouth, plus motivated lights. `convertedGlbDir` is the loose
    // converted_glb root (x3::game::convertedGlbRoot()). Safe to call once at level build;
    // if the dir/GLBs are missing it leaves the system empty (nothing drawn, no lights) so
    // the level falls back to the existing graybox + per-room light look. Returns true if
    // at least one prop loaded.
    bool build(x3::rhi::IRenderDevice& device, std::string_view convertedGlbDir,
               const CanonFloor& floor);

    // Draw all placed props (static; call alongside scene.render() each frame). No-op if
    // nothing loaded. The host gates this by the cell/hall being in the visible room set.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Per-frame: advance the flicker phase (a fluorescent cell tube that stutters). dt in
    // seconds. Cheap; mutates the cached light intensities the host feeds next frame.
    void tick(float dt);

    // The motivated PointLights for the dressed rooms (cell + hall mouth): a flickering
    // overhead tube, a red alarm wash, a cyan terminal accent, warm bunk fill. Each is
    // tagged with the room it belongs to so the host can feed only the visible set (these
    // are appended to the canon room-light feed, still under the 64-light cap). The
    // intensities reflect the current flicker phase (call tick() first).
    struct DressLight {
        x3::rhi::PointLight light;
        uint32_t            room = kNoRoom;
    };
    const std::vector<DressLight>& lights() const { return m_lights; }

    bool built() const { return m_built; }
    uint32_t propInstances() const { return (uint32_t)m_instances.size(); }
    uint32_t propsLoaded() const;

private:
    struct Asset {
        x3::asset::Model                      model;
        std::vector<x3::asset::ModelDrawable> drawables;
        bool ok = false;
    };
    struct Instance {
        uint32_t asset = 0;
        float    transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float    emissive[4]   = {0,0,0,0};   // rgb linear, w = strength (per-instance glow)
        float    tint[4]       = {1,1,1,1};   // per-instance baseColor MULTIPLIER (darken/tint)
    };
    // ATMOSPHERE: a procedural (non-GLB) mesh drawn either emissive or as a soft
    // translucent volume. Used for the volumetric light SHAFTS (glass cones from the
    // cell tube / door) and the drifting DUST MOTES caught in the light. These need
    // no asset; their geometry is built once at build() time into m_proc.
    struct ProcDraw {
        uint32_t meshIdx = 0;                 // index into m_procMeshes
        float    transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float    color[4]   = {1,1,1,1};      // baseColor factor (rgb, a = blend opacity for glass)
        float    emissive[4]= {0,0,0,0};      // rgb linear glow, w = strength
        bool     glass = false;               // true -> drawMeshGlass (soft volumetric shaft)
        // Glass surface params (WINDOW GLAZING vs the soft shafts/shadow blobs): the
        // shafts/blobs want a dead-matte non-reflective medium (rough 1 / spec 0, the
        // old hardcoded values, kept as defaults); the cell's ARMORED GLASS panes want
        // a smooth specular sheet so they catch the room lights and read as glass.
        float    glassRough = 1.0f;
        float    glassSpec  = 0.0f;
    };
    // A drifting dust mote: a tiny emissive instance whose transform is rebuilt each
    // tick() from a slow upward+lateral drift inside a light pool (so specks float in
    // the beam). Cheap: a handful, all sharing one small mesh.
    struct Mote {
        uint32_t draw;                        // index into m_proc (the ProcDraw it animates)
        float    ox, oy, oz;                  // pool origin (light pos)
        float    rx, rz;                      // lateral radius of the drift volume
        float    phase, rate;                 // drift phase / speed
        float    riseY, span;                 // vertical rise span (loops)
        float    size;                        // mote scale
    };

    // Load (cached) a converted GLB by relative path; returns the asset index (always
    // valid — a failed load yields ok=false that draws nothing).
    uint32_t load(const std::string& relPath);
    // Place an instance of a kit piece. `yaw` rotates about +Y (env_art convention: 0 =
    // faces world -Z). `scale` is uniform. The asset's local anchor (ax,ay,az) is mapped
    // to the world point (wx,wy,wz) — pass the asset's (min-Y, center-XZ) to seat a prop on
    // the floor. `emissive` (nullable) gives the instance a per-instance glow (HDR bloom).
    void place(uint32_t asset, float yaw, float scale,
               float ax, float ay, float az,
               float wx, float wy, float wz,
               const float emissive[4] = nullptr,
               const float tint[4] = nullptr);
    // Append a motivated point light tagged with `room`.
    void addLight(uint32_t room, float x, float y, float z, float range,
                  float r, float g, float b);

    // ---- ATMOSPHERE builders (procedural; no GLB) -------------------------------
    // Register a procedural mesh (built from x3::prims) on the device; returns its
    // index into m_procMeshes. Cached not needed (a handful of distinct shapes).
    uint32_t addProcMesh(x3::rhi::IRenderDevice& device, const struct ProcGeo& g);
    // Seed N drifting dust motes inside a light pool centered at (x,y,z).
    void addDustMotes(uint32_t moteMesh, int n, float x, float y, float z,
                      float rx, float rz, float riseY, float span,
                      float r, float g, float b, float glow);
    // ROUND 3 — a soft contact-shadow / AO blob lying flat on the floor under a prop, so
    // the prop grounds in the space instead of floating. `discMesh` is the shadow disc
    // (makeShadowDisc); (x,z) is the floor-plane center, `y` the floor height, `radX/radZ`
    // the blob radii (m), `darkness` the center opacity (0..1, fades to 0 at the rim).
    void addShadowBlob(uint32_t discMesh, float x, float y, float z,
                       float radX, float radZ, float darkness);

    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<Asset>                       m_assetTable;
    std::vector<std::string>                 m_assetPaths;
    std::vector<Instance>                    m_instances;
    std::vector<x3::rhi::MeshHandle>         m_procMeshes; // procedural atmosphere meshes
    std::vector<ProcDraw>                    m_proc;        // shafts + motes
    std::vector<Mote>                        m_motes;       // animated motes (index into m_proc)
    std::vector<DressLight>                  m_lights;
    // Indices into m_lights of the lights whose intensity is flicker-driven, with their
    // base color (so tick() can modulate them) + a per-light phase/rate.
    struct Flicker { uint32_t idx; float baseR, baseG, baseB; float phase, rate, depth; };
    std::vector<Flicker>                     m_flickers;
    uint32_t m_shadowDisc = 0;   // ROUND 3 contact-shadow disc mesh (index into m_procMeshes)
    bool m_built = false;
};

} // namespace x3::game
