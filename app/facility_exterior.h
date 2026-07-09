#pragma once
// FACILITY EXTERIOR — the glass curtain-wall tower skin, factored out of
// app/world_hosts/host_surface_start.cpp (SEAM 2 of the world merge) so ONE
// builder produces the exterior for BOTH sites:
//
//   * --world surface: the ESCAPED-branch Act-1 landing keeps its facility
//     exactly as shipped (same constants, same pane jitter, same per-pane
//     draws) — the host now CALLS this builder instead of inlining ~400 lines.
//   * --world canonlevel: the SAME exterior wraps the REAL canonical tower
//     (app_run computes the tower's footprint from CanonFloor rooms and this
//     module builds the facade OUTSIDE the R-9 skirt), with ONE breach that
//     lines up with a doorway-style cut in an F1 room's exterior wall so the
//     player can WALK in and out of the building.
//
// What this module owns (the reusable, fiction-free pieces):
//   * terrain/apron: either the surface world's 300 m soil plate + one big
//     textured apron panel, or a RING apron + soil skirt around an arbitrary
//     footprint (canon), both with static collision;
//   * the near-black BACKING WALLS (collision) around the footprint, split
//     around the breach on the breach face;
//   * the GLASS CURTAIN WALL: per-storey translucent panes with hashed
//     micro-tilt/tint jitter (drawMeshGlass), optionally MERGED into a few
//     batched meshes (the canon tower is ~27 storeys — thousands of panes —
//     so per-pane draws would swamp the frame; the surface host keeps the
//     original per-pane path so it stays byte-identical);
//   * the white-concrete SPANDREL BANDS + parapet crown + entrance jambs +
//     the amber entrance SIGN (surface_library cc_cement_white PBR);
//   * the BREACH: an open, collision-free entry gap + glowing marker + the
//     amber light spill (exposed via spillLight() for the host's light feed),
//     plus an optional floored/walled VESTIBULE strip that carries the walk
//     from an interior wall opening out through the facade (canon);
//   * the GOLDEN-HOUR sky preset both worlds share (applyGoldenHourSky).
//
// What stays in the hosts (fiction): Sarah/rescue staging, the landed ship,
// apron props, objectives, spawns, horizon ring + city massing.
//
// Every scene entity added here is tagged kNoRoom => ALWAYS drawn under the
// canon per-room PVS cull (the same contract as the R-9 skirt panels), and
// render-only pieces never block a doorway. Game/slice code only; engine/
// stays pure. Mirrors the level_loader / surface_library neighbours.

#include "scene.h"
#include "surface_library.h"
#include "level_loader.h"     // CanonFloor (ensureOutdoorVis) + kNoRoom

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

class FacilityExterior {
public:
    // Which face of the footprint carries the entry breach (outward normal).
    enum class Face : int { MinusX = 0, PlusX = 1, MinusZ = 2, PlusZ = 3 };

    // How the ground around the facade is built.
    enum class Apron : int {
        SurfacePanel,   // the surface world's single big panel + 300 m soil plate
        Ring,           // 4 concrete panels ringing the footprint + a soil skirt
        None
    };

    struct Desc {
        // BACKING-WALL plane rectangle the curtain wall wraps (world meters).
        // For the canon tower: the rooms' overall footprint padded outward so
        // the facade clears the R-9 exterior skirt (no z-fighting).
        float x0 = 0, x1 = 0, z0 = 0, z1 = 0;
        float baseY = 0;              // facade base (== the walk-out floor level)
        float topY  = 0;              // tower top (parapet rows rise above this)

        // The entry BREACH: one open gap in the facade.
        Face  breachFace   = Face::PlusZ;
        float breachCenter = 0.0f;    // coord along the face (x for Z faces, z for X faces)
        float breachHalfW  = 2.4f;    // matches the surface world's kBreachHalfW

        // Opaque slabs. The surface facility is a hollow box (roof + interior
        // floor); the canon tower brings its own floors — floorSlab OFF there.
        bool  roofSlab  = true;
        bool  floorSlab = true;
        float roofLift  = 0.0f;       // raise the roof slab (clear canon F7 ceiling lids)

        // BREACH VESTIBULE (canon): a floored + side-walled walk strip running
        // `vestibuleDepth` inward from the facade plane, so the player crosses
        // the interstice between the room's cut wall and the facade breach
        // without stepping into the void. 0 = none (the surface facility's
        // interior floor slab already carries the walk).
        float vestibuleDepth = 0.0f;
        float vestibuleHalfW = 1.7f;

        // Apron / terrain.
        Apron apron = Apron::SurfacePanel;
        // SurfacePanel mode (the surface world's exact panel):
        float apronPanelW = 70.0f, apronPanelD = 66.0f;
        float apronAnchorX = 0.0f, apronAnchorY = 0.02f, apronAnchorZ = 0.0f;
        bool  terrainPlate = false;   // the 300 m soil plate + collision (surface)
        // Ring mode: concrete ring width + soil skirt outer reach (both collide).
        float apronOut = 24.0f;
        float soilOut  = 150.0f;

        // Pane batching: merge the panes into kMergeGroups meshes (one
        // drawMeshGlass each) instead of one draw call per pane. The per-pane
        // TILT jitter survives (baked into the merged verts); tint/roughness
        // jitter becomes per-GROUP. Use for big towers (canon); the surface
        // host keeps false for the original per-pane look.
        bool  mergePanes = false;

        // Surface-library set names (both worlds ship these).
        std::string towerSet = "cc_cement_white";
        std::string apronSet = "sr_concrete_01";

        // canon only: the room index behind the breach (ensureOutdoorVis seeds
        // the visible set with it while the player stands outdoors).
        uint32_t breachRoomHint = kNoRoom;
    };

    // Build everything. Scene entities (backing walls / slabs / soil / marker /
    // vestibule) are tagged kNoRoom + Tag::Static; collision goes to `physics`.
    // Bands / panes / apron panels / the sign are RETAINED draw lists — call
    // draw() once per frame after the scene render. Logs its build cost.
    // `sharedLib` (optional): reuse an already-populated SurfaceLibrary (the
    // canon host passes RoomDressing's, whose GPU textures already hold the
    // recipe concrete sets — no duplicate multi-MB PNG decodes at boot). Must
    // outlive this object. Null => this object mounts its own (surface host).
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const Desc& d,
               SurfaceLibrary* sharedLib = nullptr);

    // Draw the retained facade pieces (glass panes, apron panels, spandrel
    // bands, the amber entrance sign). No-op until built.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // The warm amber light leaking out of the breach (the surface world's
    // exact spill values, positioned at this facade's breach). Append to the
    // frame's point-light feed.
    x3::rhi::PointLight spillLight() const;

    // The golden-hour sky both worlds share (the surface world's exact params).
    static void applyGoldenHourSky(x3::rhi::IRenderDevice& device);

    // OUTDOOR CULL GUARD (canon): when the camera stands OUTSIDE every room
    // (roomAt == kNoRoom — on the apron), the portal flood already seeds from
    // the nearest room so the world never blanks; additionally force the
    // breach room into the visible set so the interior read through the open
    // breach never pops based on which room center happens to be nearest.
    void ensureOutdoorVis(const CanonFloor& floor, float x, float y, float z,
                          std::vector<uint32_t>& visRooms) const;

    bool     built() const { return m_built; }
    uint32_t paneCount() const { return m_paneCount; }
    uint32_t bandCount() const { return (uint32_t)m_bands.size(); }

private:
    struct BandDraw { x3::rhi::MeshHandle mesh; float xform[16]; };
    struct GlassPanelDraw {
        float xform[16];
        float base[4];
        x3::rhi::IRenderDevice::GlassMaterial mat;
    };
    struct MergedGlass {
        x3::rhi::MeshHandle mesh;
        float base[4];
        x3::rhi::IRenderDevice::GlassMaterial mat;
    };
    struct ApronDraw { x3::rhi::MeshHandle mesh; float xform[16]; };

    SurfaceLibrary m_surflib;           // owned library (used when no sharedLib)
    SurfaceLibrary* m_lib = nullptr;    // the ACTIVE library (owned or shared)
    const SurfaceSet* m_sTower = nullptr;
    const SurfaceSet* m_sApron = nullptr;

    std::vector<BandDraw>       m_bands;
    std::vector<GlassPanelDraw> m_panes;      // per-pane path (surface)
    std::vector<MergedGlass>    m_merged;     // batched path (canon)
    std::vector<ApronDraw>      m_aprons;
    x3::rhi::MeshHandle  m_glassPanelMesh{};
    x3::rhi::TextureHandle m_glassPanelTex{};
    x3::rhi::MeshHandle  m_signMesh{};
    float m_signXform[16] = {};
    bool  m_hasSign = false;

    Desc m_desc{};
    bool m_built = false;
    uint32_t m_paneCount = 0;
};

} // namespace x3::game
