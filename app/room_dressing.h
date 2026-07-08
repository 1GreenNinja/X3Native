#pragma once
// WAVE-3 ROOM-DRESSING RECIPES — scale the calibrated cell treatment to EVERY room
// on a canon floor, by room TYPE (WAVE3_WAVE4_PLAN §3.1, ART_BIBLE §2/§3/§6).
//
// The data-driven floor builds graybox shells + generic warm per-room lights; only
// Jake's cell got the hand-calibrated CellDressing pass. This system dresses the
// OTHER 52 rooms from a small recipe table keyed on canonical room name/type:
//   * REAL PBR wall/floor/ceiling panels from the SurfaceLibrary (ART_BIBLE §4
//     realism mandate — authored texture sets, not tinted kit boxes), tiled with
//     the 0.14 m inset law and OPENING-AWARE segmentation (wall runs are cut
//     around every resolved doorway span — Law 1: no panel ever covers a door).
//   * Motivated lights per the bible zone palette: ONE key, supports at <= half
//     energy, exactly ONE accent hue — pushed as CanonLights (room-tagged) so the
//     host's existing visible-room light selection budgets them automatically.
//   * 1-2 hero props from the converted kit per recipe (yaw-only placement law),
//     grounded with contact-shadow discs.
//   * A per-zone FOG tint (AD-1 painterly levers): the host calls
//     applyZoneAtmosphere(eyeRoom) each frame and the fog re-tints as the player
//     crosses zone boundaries (teal halls / amber detention / green labs).
//   * Teal floor guide strips down the hall/corridor leading lines (§3.2
//     wayfinding; world-space name plates need a text-in-world path that does not
//     exist yet — reported, not faked).
//
// Jake's cell (beats.jakeCell) is UNTOUCHED — it stays the frozen reference.
// Purely visual overlay: no collision, no gameplay. Missing GLB/texture set ->
// that piece is skipped (graybox remains), never a boot failure.

#include "level_loader.h"     // CanonFloor / CanonRoom / CanonDoorway / CanonLight
#include "surface_library.h"  // SurfaceLibrary / SurfaceSet

#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

class RoomDressing {
public:
    // Build recipes for every classifiable room on the floor (skips jakeCell, the
    // deep Cave/Hidden Sub-Level rooms, and rooms no recipe matches). Returns true
    // if at least one room was dressed. surfaceLibDir = <assets>/surface_library;
    // convertedGlbDir = x3::game::convertedGlbRoot().
    bool build(x3::rhi::IRenderDevice& device,
               std::string_view surfaceLibDir, std::string_view convertedGlbDir,
               const CanonFloor& floor, const CanonBeats& beats);

    // Draw the panels/props/strips of every DRESSED room in `visibleRooms` (the
    // host's per-frame PVS set — same gating the scene cull uses).
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const std::vector<uint32_t>& visibleRooms) const;

    // Per-frame zone atmosphere: sets the AD-1 depth fog to the recipe tint of the
    // zone containing `eyeRoom`, only when the zone actually changes (no redundant
    // setFog spam). Call BEFORE the r_fog* cvar override block so explicit cvar
    // overrides still win. kNoRoom / undressed rooms fall back to the detention tint.
    void applyZoneAtmosphere(x3::rhi::IRenderDevice& device, uint32_t eyeRoom);

    // Room-tagged recipe lights (key/fill/accent per dressed room). The host
    // APPENDS these to the buildCanonLights list after filtering out the generic
    // warm light for rooms where hasRecipe() is true (the recipe owns that room's
    // light statement — bible: one key per room, not key + generic wash).
    const std::vector<CanonLight>& lights() const { return m_lights; }
    bool hasRecipe(uint32_t room) const {
        return room < m_roomZone.size() && m_roomZone[room] != 0;
    }

    uint32_t roomsDressed() const { return m_roomsDressed; }

    // W9-2 (desc-mechanics Tier B #7): kill every dressed EMISSIVE statement in
    // `room` — prop instance emissive scales AND the procedural glow/glass quads
    // (the Portal Chamber's layered portal glass, the Energy Nexus core) drop to
    // dead. Visual-only, idempotent. Returns how many draws were dimmed.
    uint32_t killRoomEmissives(uint32_t room);

private:
    struct Panel {                    // one textured surface panel, prebuilt
        uint32_t room = kNoRoom;
        uint32_t set  = 0;            // index into m_sets
        x3::rhi::MeshHandle mesh{};
        float    transform[16] = {};
    };
    struct PropInst {                 // one placed kit prop (cell_dressing pattern)
        uint32_t room  = kNoRoom;
        uint32_t asset = 0;           // index into m_assets
        float    transform[16] = {};
        float    emissive[4] = { 0, 0, 0, 0 };  // [3] SCALES material emissive (R5 law)
        float    tint[4]     = { 1, 1, 1, 1 };
    };
    struct ProcDraw {                 // guide strips + contact-shadow discs
        uint32_t room = kNoRoom;
        x3::rhi::MeshHandle mesh{};
        bool  glass = false;          // true = shadow disc via the glass pass
        float color[4]    = { 1, 1, 1, 1 };
        float emissive[4] = { 0, 0, 0, 0 };
        float transform[16] = {};
    };
    struct Asset {                    // one loaded kit GLB (cached)
        x3::asset::Model model;
        std::vector<x3::asset::ModelDrawable> drawables;
        bool ok = false;
    };

    uint32_t loadAsset(const std::string& rel);
    void     placeProp(uint32_t room, uint32_t asset, float yaw, float s,
                       float ax, float ay, float az, float wx, float wy, float wz,
                       const float emissive[4], const float tint[4]);
    x3::rhi::MeshHandle quadMesh(x3::rhi::IRenderDevice& device,
                                 float w, float h, float tileMeters);

    std::unique_ptr<x3::asset::IAssetSource>  m_assets;
    std::unique_ptr<x3::asset::IModelLoader>  m_loader;
    SurfaceLibrary                            m_surf;
    std::vector<const SurfaceSet*>            m_sets;      // stable ptrs into m_surf cache
    std::vector<Asset>                        m_assetTable;
    std::vector<std::string>                  m_assetPaths;
    std::vector<Panel>                        m_panels;
    std::vector<PropInst>                     m_props;
    std::vector<ProcDraw>                     m_proc;
    std::vector<CanonLight>                   m_lights;
    std::vector<uint8_t>                      m_roomZone;   // per-room Zone id (0 = none)
    std::vector<x3::rhi::IRenderDevice::FogParams> m_zoneFog; // fog per Zone id
    // quad mesh dedupe: key = quantized (w,h,tile)
    std::vector<std::pair<uint64_t, x3::rhi::MeshHandle>> m_quadCache;
    uint32_t m_roomsDressed = 0;
    int      m_lastZone     = -1;
};

} // namespace x3::game
