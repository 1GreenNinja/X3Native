// app/space/ship_interior.h
//
// S5 — the WALKABLE, DATA-DRIVEN ship interior (Star Trek "static frame" model).
//
// The interior is STATIC at the scene origin: the player walks around INSIDE the
// ship; "motion through space" is the ENVIRONMENT moving past the windows (decision
// 2.4 — S0 owns the environment transform; windows are a later lane, S6). This
// system only builds + renders the walkable shell + collision + station markers.
//
// DATA-DRIVEN so scope SCALES WITH SHIP CLASS (decision 2.3/2.8): the same code
// path runs a Small one-room cockpit, a Large multi-room ship, or a Huge multi-deck
// vessel — the only difference is the ShipManifest fed to build(). Rooms become
// floor/wall/ceiling shells (from x3::prims boxes, reused like the other showcases),
// doors are stored as gaps/markers, stations get a marker prop, and window
// placements are STORED (not rendered) for S6 to consume.
//
// Game/slice code only — engine/ stays pure. Reuses the public Scene / IRenderDevice
// / IPhysicsWorld API exactly like secret_room / club1127. It REUSES (never modifies)
// app/player.* for walking; the host spawns a Player inside a built interior.
#pragma once

#include "../scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x3::space {

// Ship class drives interior SCOPE (decision 2.3): Small = a cockpit shell, Large =
// a multi-room ship, Huge = a multi-deck vessel. All three run the same ShipInterior
// system off a ShipManifest — only the manifest's room/door/station counts differ.
enum class ShipClass { Small, Large, Huge };

// A walkable volume. `boundsMin`/`boundsMax` are the world-space AABB of the room's
// INTERIOR floor space (x,y,z). build() wraps it in floor/wall/ceiling shells; the
// player walks within these bounds.
struct Room { std::string name; float boundsMin[3]; float boundsMax[3]; };

// A doorway connecting two rooms. `pos` is the world-space center of the opening,
// `size` is {width, height}; roomA/roomB index into ShipManifest::rooms. build()
// leaves a real GAP in the shared wall here so the player can pass between rooms.
struct Door { float pos[3]; float size[2]; uint32_t roomA, roomB; };

// A crew station (helm / nav / repair / weapons). `pos` is the world-space floor
// position of the console, `yaw` the facing (radians). build() drops a small
// emissive console marker prop here (and the host MAY place a crew NPC at it).
struct Station { std::string kind; float pos[3]; float yaw; };

// A ship-class interior layout. DATA-DRIVEN so one system spans every ship class.
struct ShipManifest {
    ShipClass shipClass = ShipClass::Small;
    std::vector<Room>    rooms;
    std::vector<Door>    doors;
    std::vector<Station> stations;
    // Window placements consumed LATER by S6 (the moving-environment view). Stored
    // here but NOT rendered by ShipInterior — each is {x,y,z, w,h, yaw}.
    std::vector<std::array<float, 6>> windows;
};

// Builds + renders the walkable interior for a ShipManifest. One instance owns the
// render meshes/textures + the static collision bodies + the station-marker entities
// it creates; shutdown() releases the physics bodies.
class ShipInterior {
public:
    // Build render geometry (floor/walls/ceiling per room from prim boxes) + static
    // collision (boxes so the Player can't walk through walls) + station marker
    // props, all into `scene`. Doorways are cut as gaps in the shared walls. Window
    // placements are stored only (S6 renders them later). Call once.
    void build(x3::rhi::IRenderDevice& device, x3::game::Scene& scene,
               x3::phys::IPhysicsWorld& physics, const ShipManifest& manifest);

    // Per-frame draw of the interior. Thin wrapper over scene.render() (the shells +
    // markers are Scene entities), so the host can call either this or scene.render()
    // directly — both draw the same set. Kept for API symmetry with the spec.
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                x3::game::Scene& scene);

    // The manifest this interior was built from (round-trips what build() consumed).
    const ShipManifest& manifest() const { return m_manifest; }

    // Counts for the self-test / HUD.
    uint32_t roomCount()    const { return (uint32_t)m_manifest.rooms.size(); }
    uint32_t stationCount() const { return (uint32_t)m_manifest.stations.size(); }
    uint32_t entityCount()  const { return m_entityCount; }   // scene entities build() added
    bool     built()        const { return m_built; }

    // A sensible spawn point INSIDE the first room (feet position) so the host can
    // drop a Player into the cockpit immediately. Center of room 0, on its floor.
    x3::phys::Vec3 spawnPoint() const { return m_spawn; }

    // Release the static collision bodies created by build(). Safe before
    // physics->shutdown(); idempotent.
    void shutdown(x3::phys::IPhysicsWorld& physics);

    // ---- Built-in manifests -------------------------------------------------
    // A small cockpit + a short corridor (2 rooms, 1 connecting door, a helm +
    // a nav station, 2 windows). Drives the showcase + the --test-ship-interior.
    static ShipManifest makeSmallCockpit();

private:
    ShipManifest m_manifest;
    bool         m_built = false;
    uint32_t     m_entityCount = 0;
    x3::phys::Vec3 m_spawn{};

    // Static collision bodies (walls/floor/ceiling) — torn down in shutdown().
    std::vector<x3::phys::BodyId> m_bodies;
    // Render meshes/textures owned by this interior (freed in shutdown()).
    std::vector<x3::rhi::MeshHandle>    m_meshes;
    std::vector<x3::rhi::TextureHandle> m_textures;
};

// ===========================================================================
// FireflyCockpit — the "used future" SHOWCASE dressing for --world ship-interior.
//
// ShipInterior (above) owns the WALKABLE shell + collision (graybox boxes). It is
// data-driven + ship-class-agnostic, so it must stay neutral. The Firefly look —
// real modular SM_* sci-fi geometry, a pilot's console, ceiling pipes, and WARM
// amber light fixtures (Serenity's lived-in cockpit) — is a SHOWCASE concern,
// kept here as a separate, purely-VISUAL overlay that draws on top of (and instead
// of) the graybox so the brief's art direction doesn't pollute the reusable system.
//
// It loads the ModularSciFi_Interior/SM_*.glb kit (the same pieces env_art.cpp
// uses), places them into a small angled cockpit, and registers WARM point lights
// at each Light_A fixture. Per-asset fallback: a missing GLB is simply not drawn.
// REUSES the public IModelLoader / IAssetSource / IRenderDevice API only.
class FireflyCockpit {
public:
    // Load the SM_* kit from `convertedGlbDir` (e.g. convertedGlbRoot()) and place
    // the cockpit geometry + props + warm light fixtures. Call once. Returns true
    // if at least the floor/wall kit loaded (false -> caller keeps the graybox).
    bool build(x3::rhi::IRenderDevice& device, const std::string& convertedGlbDir);

    // Draw all placed cockpit instances (static; call each frame before the windows).
    void render(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // Draw the forward SPACE pane (the view out the cockpit window: a deep, mostly-
    // dark star/nebula field at a controlled, NON-blooming emissive so the stars read
    // as points on black space — the Serenity window — not a white sheet). `panSec`
    // gently drifts the field so the ship reads as moving. Call after render(), before
    // the viewmodel. Placed at the forward hull opening (z = -3).
    void renderWindow(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      float panSec) const;

    // Warm amber point lights at the ceiling fixtures (the Firefly signature). Feed
    // these to IRenderDevice::setPointLights (merge with the window light-bleed).
    const std::vector<x3::rhi::PointLight>& warmLights() const { return m_warm; }

    uint32_t assetsLoaded() const;
    uint32_t instanceCount() const { return (uint32_t)m_instances.size(); }
    bool     ok() const { return m_ok; }

private:
    struct Asset {
        x3::asset::Model                      model;
        std::vector<x3::asset::ModelDrawable> drawables;
        bool ok = false;
    };
    struct Inst {
        uint32_t asset = 0;
        float    transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float    emissive[4] = {0,0,0,0};
    };
    uint32_t loadAsset(const std::string& relPath);
    void     addInstance(uint32_t a, const float m[16], const float emissive[4] = nullptr);

    std::unique_ptr<x3::asset::IAssetSource> m_assets;
    std::unique_ptr<x3::asset::IModelLoader> m_loader;
    std::vector<Asset>                       m_table;
    std::vector<std::string>                 m_paths;
    std::vector<Inst>                        m_instances;
    std::vector<x3::rhi::PointLight>         m_warm;
    bool                                     m_ok = false;

    // Forward space window (self-owned so brightness is controlled — the S6 ShipWindows
    // pane blooms to white at this size/closeness). A baked star/nebula quad.
    x3::rhi::MeshHandle    m_winMesh{};
    x3::rhi::TextureHandle m_winTex{};
    std::array<float, 6>   m_winPlace{};   // {x,y,z, w,h, yaw}
    bool                   m_winOk = false;
};

// Headless self-test (--test-ship-interior, >=6 checks). Uses HeadlessRenderDevice +
// a fresh physics world (no window / Vulkan):
//   T1 makeSmallCockpit() returns >=1 room AND >=1 station (data-driven manifest);
//   T2 build() populates the Scene with entities (drawnCount would be > 0);
//   T3 manifest() round-trips the room/station/door/window counts that were fed in;
//   T4 a Large multi-room manifest builds MORE rooms than the Small cockpit (scope
//      scales with ship class on the SAME system);
//   T5 the spawn point lies INSIDE room 0's bounds (player drops into the cockpit);
//   T6 shutdown() is clean + idempotent (entities hidden, bodies released, no crash
//      on a second call).
// Logs PASS/FAIL T#; returns true iff all pass. Lives in ship_interior.cpp.
bool runShipInteriorSelfTest();

} // namespace x3::space
