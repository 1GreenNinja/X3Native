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
#include "keypad.h"      // KeypadHandles (phantom-door keypad flash sequencing)

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

    // ===== THE MASTER ACCESS (owner order 2026-07-25: "backup code 7762 opens
    // the door for me"). The unnumbered (4.5-height) phantom door is a REAL
    // code-locked master door: behind it an L-connector runs east then north to a
    // sanctioned mouth cut in the 4.5 cavern's -Z rock wall — the second route
    // into the hidden level, alongside the elevator. The code (7762) has NO
    // in-world clue, deliberately: it is the owner's personal key. 4545 on this
    // door still answers the sublevel tell and does NOT open it.
    //
    // The plan lives HERE (pure data) so the stairwell builder, Canon45's wall
    // cut and the level lint's seal gate all agree on the one sanctioned opening.
    struct MasterAccess {
        bool  present  = false;
        float landingY = 0;          // the unnumbered landing's floor Y
        float floorY   = 0;          // connector floor top == the 4.5 cavern floor plane
        // Sanctioned L-connector INTERIOR boxes (world XZ; y = floorY..floorY+kMasterH):
        float aX0 = 0, aX1 = 0, aZ0 = 0, aZ1 = 0;   // leg A: east from the shaft door
        float bX0 = 0, bX1 = 0, bZ0 = 0, bZ1 = 0;   // leg B: north to the cavern wall's OUTER face
        float mouthX0 = 0, mouthX1 = 0;             // cut span in the cavern -Z wall (== bX0..bX1)
        float envZ0 = 0;                            // cavern envelope z0 (the cut plane)
    };
    MasterAccess master;
    static constexpr float kMasterH = 2.4f;   // connector interior height (== kDoorH)
};

// Derive the plan from the loaded tower (returns valid()==false when the tower data
// lacks the expected floors). Pure — no scene/device.
StairwellLayout stairwellLayout(const CanonFloor& floor);

class FacilityStairwell {
public:
    // ===== THE SERVICE-VOID CODE (feat/secret-code-clues) =====================
    // 4545 is a REAL code now — taught in-world by Maint. Chief Okafor's work
    // order on the F1 stairwell-entrance holo-terminal ("re-keyed 45-45 after
    // the incident"). It is a LORE BEAT, not a key: every phantom door ACCEPTS
    // it (keypad flashes GREEN, then AMBER with a denial) and STAYS SHUT — the
    // voids are sealed geometry and the 4.5 seal is absolute. The one door that
    // answers DIFFERENTLY (the unnumbered slab at level 4.5's height) is the
    // chain link that sends the player to the elevator + the chief engineer.
    static constexpr int kServiceCode = 4545;

    // ===== THE MASTER BACKUP CODE (owner order 2026-07-25) ====================
    // 7762 physically OPENS the unnumbered door (standard SM_Door_A slide via the
    // DoorSystem — the door carries this code) onto the master L-connector into
    // 4.5. UNDOCUMENTED IN-WORLD: no terminal, no chatter, no graffiti teaches
    // it — the owner's personal key. On the other 17 phantom doors 7762 is just
    // another wrong code (plain red reject; their voids are real voids). The
    // door auto-closes + RE-LOCKS behind the rider (update()) so 4.5 is never
    // casually propped open.
    static constexpr int kMasterCode = 7762;

    // What a submitted code meant at a phantom door. Pure classification —
    // shared by the host hook and the headless self-test (KP7/KP8).
    enum class CodeResponse {
        NotHandled,     // wrong code / not near a phantom door: existing red reject
        ServiceVoid,    // 4545 on a NUMBERED phantom: "SERVICE VOID - NO ATMOSPHERE"
        SublevelTell,   // 4545 on the UNNUMBERED (4.5-height) door: "SEE CHIEF ENGINEER"
    };
    static CodeResponse classifyCode(int code, bool sublevelTell) {
        if (code != kServiceCode) return CodeResponse::NotHandled;
        return sublevelTell ? CodeResponse::SublevelTell : CodeResponse::ServiceVoid;
    }

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

    // True iff `entity` is one of the stairwell's locked phantom door slabs (the
    // host's E-aim path uses this to bark the service-void line instead of the
    // generic "enter code N" prompt — which would leak the code on the HUD).
    bool isPhantomDoorEntity(uint32_t entity) const;

    // Host hook for a submitted keypad code near the stairwell. Finds the nearest
    // phantom door within `range` of `eye`; on 4545 starts the keypad flash
    // sequence (GREEN 0.7 s -> AMBER 3 s -> red) and returns the response — the
    // door does NOT open (the void stays sealed; DoorSystem never sees 4545).
    // Any other code, or no phantom door in reach, returns NotHandled so the
    // host's existing red-reject path runs unchanged.
    CodeResponse submitCode(const x3::phys::Vec3& eye, int code, Scene& scene,
                            float range = 3.5f);

    // Advance the keypad flash sequence + the master door's auto-close/re-lock
    // (the unnumbered door shuts and re-arms once the rider is > 6 m away, so
    // level 4.5 never sits casually propped open). `doors` = the DoorSystem the
    // phantom slabs were registered in; null skips the door logic.
    void update(float dt, Scene& scene, DoorSystem* doors,
                const x3::phys::Vec3& playerPos);

    // Capture hook (X3_STAIR_DEMO): run the 4545 response on a phantom door
    // without a player — `tell` picks the unnumbered 4.5-height door. Returns the
    // response (NotHandled when unbuilt / no such door).
    CodeResponse demoSubmit(bool tell, Scene& scene);

private:
    // One locked phantom-landing door + its keypad, registered at build.
    struct PhantomDoor {
        uint32_t       doorIndex = 0;        // index into the host DoorSystem
        uint32_t       entity    = kNoLink;  // the door slab's Scene entity
        KeypadHandles  keypad;               // the wall keypad beside it
        x3::phys::Vec3 center{};             // doorway center (proximity match)
        bool           sublevelTell = false; // the unnumbered door at 4.5's height
    };
    std::vector<PhantomDoor> m_phantoms;
    int   m_flashIdx  = -1;     // phantom whose keypad is mid-flash (-1 = idle)
    float m_flashT    = 0.0f;   // seconds into the flash sequence
    bool  m_built = false;
    SurfaceLibrary m_lib;

    void startFlash(int idx, Scene& scene);
};

} // namespace x3::game
