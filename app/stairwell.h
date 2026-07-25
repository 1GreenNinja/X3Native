// THE FACILITY STAIRWELL (fix/spire-hollow-core, owner feature 2026-07-25):
// "we can have stairways accessing the other floors!!! and we should!!! Open
// stairway, top to bottom."
//
// An open ZIGZAG SWITCHBACK stairwell connecting the NORMAL floors (F1..F7) on the
// tower's west edge: two flight lanes side by side, half-landing at each end, an
// OPEN CENTRAL WELL between the lanes (railed parapets — the player can look up/down
// the whole run), a landing at every ~half-story, and a doorway-connector into a real
// room on every floor. Owner refinements:
//   * BLACK RUBBER NOSING on every tread lip (institutional safety strip; honest
//     matte material — near-black albedo, high roughness, zero metal).
//   * NO BLANK-WALL LANDINGS: every north landing that does NOT open onto a real
//     floor gets a LOCKED KEYPAD DOOR instead (sealed behind — a door that will not
//     open, with a keypad that rejects you). This includes the landing at level
//     4.5's height: the hidden floor's tell is a door with no number, not a blank
//     wall. A future secret code can open one with zero rework (DoorSpec::code).
//   * Painted floor numbers on real-floor landings only — the numbering silently
//     skips 4.5.
//
// CANON GUARD: the stairwell NEVER opens into level 4.5 (owner canon: hidden,
// elevator-only). The shaft sits west of the tower rooms, outside the 4.5 cavern
// envelope; the locked landings are sealed faces. level_lint's stairwell gate
// asserts both properties from this module's layout (see stairwellLayout).
#pragma once

#include "level_loader.h"
#include "surface_library.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

class DoorSystem;

// The computed stairwell plan — pure data, derived from the loaded tower only, so
// the builder, the host wiring (extraBreaches) and the lint gate all agree.
struct StairwellLayout {
    bool valid = false;

    // Shaft EXTERIOR box (x/z; y covered by baseY/topY).
    float sx0 = 0, sx1 = 0, sz0 = 0, sz1 = 0;
    float baseY = 0, topY = 0;

    // One entry per REAL floor served (ascending Y).
    struct FloorEntry {
        int      floorNum = 0;
        uint32_t room     = kNoRoom;   // the room the connector opens into
        float    floorY   = 0;         // landing floor Y == that room's floor
        float    roomWallX = 0;        // the room's -X wall plane (connector far end)
    };
    std::vector<FloorEntry> floors;

    // Every NORTH-end landing, ascending: real floors AND phantom (locked-door)
    // landings. floorNum > 0 for real floors, -1 for phantom.
    struct NorthLanding { float y = 0; int floorNum = -1; };
    std::vector<NorthLanding> north;

    // The connector door cut, shared by builder + host + lint:
    static constexpr float kDoorZ     = -17.9f;   // cut center (Z) on the +X wall / room walls
    static constexpr float kDoorHalfW = 0.8f;     // cut half-width
    static constexpr float kDoorH     = 2.4f;     // opening height above the landing
};

// Derive the plan from the loaded tower (returns valid()==false when the tower data
// lacks the expected floors). Pure — no scene/device.
StairwellLayout stairwellLayout(const CanonFloor& floor);

class FacilityStairwell {
public:
    // Build the full stairwell: shaft, flights (rubber-nosed treads), railed open
    // well, landings, per-floor connectors (sealing onto the breaches the host
    // registered from the same layout), locked keypad doors on phantom landings,
    // painted floor numbers, per-landing practicals (appended to canonLights as
    // un-roomed range-gated lights). `doors` receives the locked phantom slabs.
    void build(const StairwellLayout& lay, CanonFloor& floor, Scene& scene,
               x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               DoorSystem* doors, const std::string& surfaceLibRoot,
               std::vector<CanonLight>& canonLights);

    bool built() const { return m_built; }

private:
    bool m_built = false;
    SurfaceLibrary m_lib;
};

} // namespace x3::game
