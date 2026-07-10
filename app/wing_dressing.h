#pragma once
// EFLZ FLOORS 2-7 WEST-WING ROOM ART PASS — themed recipe dressing for the Spire's
// per-floor identity interiors (the "glass office building" the F2-F7 experience plays
// on). See app/level1.cpp buildLevel1 (the collision graybox) + app/room_dressing.*
// (the WAVE-3 recipe engine this reuses).
//
// The F2-F7 west wings are authored as collision graybox from the SHARED kWingRooms
// table (level1WingRooms()) — NOT the data-driven CanonFloor the RoomDressing recipe
// engine keys on. This class is the ADAPTER: it synthesizes a CanonFloor whose rooms
// ARE the wing rooms (world coords from the shared table + kFloors, a doorway per room
// from its door side so the opening-aware panel segmentation cuts around every door),
// then runs the EXISTING RoomDressing over it. Every room's `name` routes a recipe so
// each reads as a distinct AAA-dressed space — labs green + tanks, the server room cold
// cyan + racks, the drone bay vast industrial + amber caution, the exec boardroom brass
// + holo art. Reuses all of RoomDressing's calibrated laws (SurfaceLibrary PBR panels
// with the 0.14 m inset, motivated lights, hero props, per-zone fog) — no new dressing
// math. Purely visual overlay: no collision, no gameplay; missing GLB/texture -> that
// piece is skipped (graybox remains), never a boot failure.

#include "level_loader.h"     // CanonFloor / CanonRoom / CanonDoorway / RoomDressing sees it
#include "room_dressing.h"    // the WAVE-3 recipe engine (reused wholesale)

#include <cstdint>
#include <string_view>
#include <vector>

namespace x3::game {

class WingDressing {
public:
    // Build the synthetic wing floor from the shared kWingRooms table and run the recipe
    // dressing over it. surfaceLibDir = <assets>/surface_library; convertedGlbDir =
    // convertedGlbRoot(). Safe to call once at level build; returns true if >=1 room
    // dressed (false = missing assets -> the graybox wings simply stay bare).
    bool build(x3::rhi::IRenderDevice& device,
               std::string_view surfaceLibDir, std::string_view convertedGlbDir);

    // Draw the dressed panels/props/strips of the wing rooms on the eye's CURRENT floor
    // (a cheap floor-band gate — the player only ever stands on one plate). No-op until
    // built. Call alongside the other world-extra draws each frame.
    void draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              const x3::phys::Vec3& eye) const;

    // Re-tint the depth fog to the wing zone the eye is inside (only on a zone change —
    // no redundant setFog). No-op when the eye is not inside any wing room (the fog is
    // left exactly as the host set it, so the arrival halls are untouched). Call from
    // tick() with the cached device.
    void applyEyeFog(x3::rhi::IRenderDevice& device, const x3::phys::Vec3& eye);

    // Append the motivated recipe point lights (per-room key/fill/accent) of the wing
    // rooms on the eye's CURRENT floor into `out` (NOT cleared). These are the lights the
    // dressing authored to sit over each room's props — feed them to setPointLights so the
    // rooms read with their zone key (medical green / cyber cyan / drone amber / ...).
    // Floor-gated (same band as draw) so the active count stays well under the light cap.
    // Returns the number appended.
    uint32_t collectFloorLights(const x3::phys::Vec3& eye,
                                std::vector<x3::rhi::PointLight>& out) const;

    uint32_t roomsDressed() const { return m_dress.roomsDressed(); }
    bool     built() const { return m_built; }

private:
    // The wing room whose XZ + floor-Y span contains `eye`, or kNoRoom.
    uint32_t eyeRoom(const x3::phys::Vec3& eye) const;

    CanonFloor   m_floor;   // synthetic: rooms == the F2-F7 wing rooms
    RoomDressing m_dress;   // the recipe engine, run over m_floor
    struct RoomBounds { float x0, x1, z0, z1, y0, y1; };
    std::vector<RoomBounds> m_bounds;   // parallel to m_floor.rooms (eye resolution)
    bool m_built = false;
};

// Headless self-test (--test-wingdressing). Builds the synthetic wing floor on a
// HeadlessDevice, asserts every wing room parses + classifies + dresses (no ZNone
// holes), the doorway-per-room drives an opening cut, and the eye-room / floor-band
// resolution picks the right plate. Logs PASS/FAIL W#, returns true iff all pass.
bool runWingDressingSelfTest();

} // namespace x3::game
