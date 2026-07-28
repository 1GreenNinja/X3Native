#pragma once
// THE DESCENT FALL — interactive layer (feat/descent-fall). Tim's design: instead of
// walking a switchback ramp down to Club 1127, you FALL down a vertical shaft through
// the strata and land in a DARK ROOM just above the club, where a COMPUTER TERMINAL
// (lore + the door code) and a code-locked KEYPAD DOOR lead down a HALL to an ELEVATOR
// that takes the last leg INTO the club.
//
// The GEOMETRY (shaft, dark room, hall, elevator alcove, side-shoots) is built by
// club_bedrock.cpp buildEarthTunnels(), which publishes the world positions into a
// DescentFallLayout. THIS module adds the INTERACTIVE props on top of that geometry
// and owns the beat's runtime:
//   * FALL + CATCH — a scripted fast freefall (adapted from ElevState::Freefall:
//     "accelerate down hard") that the host applies to the player as they drop the
//     shaft, with a slowdown/CATCH volume in the last ~10 m so they land SAFELY on
//     the dark-room floor (never dead, never on the dance floor).
//   * COMPUTER TERMINAL — a HoloTerminal (the VIGIL/detention terminal mechanic) glowing
//     in the dark room; its readout carries the lore + reveals/hints the keypad code.
//   * KEYPAD DOOR — a code-locked sliding door (the secret-room keypad mechanic: a
//     HoloTerminal whose submit sink unlock+opens a DoorSystem door) in the room's -X
//     wall; the correct code opens it onto the hall.
//   * ELEVATOR — an ElevatorSystem car in the alcove at the hall's end; boarding it
//     rides the final leg down to the club floor.
//
// Game/slice code only; engine/ stays pure. Reuses HoloTerminal + DoorSystem +
// ElevatorSystem verbatim.
#include "scene.h"
#include "club_bedrock.h"     // DescentFallLayout
#include "holo_terminal.h"
#include "door.h"
#include "elevator.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/audio/IAudioSystem.h"

#include <string>
#include <string_view>

namespace x3::game {

// The keypad code that opens the dark-room door into the hall. ASSUMPTION (Tim can
// refine): the computer terminal reveals/hints this; we use 1127 — the canonical
// Club 1127 code — since the door ultimately leads into Club 1127 (the secret-room
// cell hatch uses the distinct 1278; the elevator disco code is also 1127).
inline constexpr const char* kDescentDoorCode = "1127";

class DescentFall {
public:
    // Scripted fall phases (mirrors the elevator's Freefall -> catch -> settle beat).
    enum class Phase : uint32_t {
        Idle = 0,    // not falling (at the mouth or already landed)
        Falling,     // accelerating down the shaft (rock/strata rushing up)
        Catching,    // in the last ~10 m: the updraft/grav-damp decelerates the drop
        Landed       // feet on the dark-room floor, safe
    };

    // Build the interactive props onto the already-built descent geometry. `layout`
    // comes from buildEarthTunnels(). `modelDir` is the loose-GLB dir (unused today;
    // reserved). `audio` may be null (headless). Call once.
    void build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const DescentFallLayout& layout,
               std::string_view modelDir = "", x3::audio::IAudioSystem* audio = nullptr);

    // ---- THE FALL (scripted; the host applies it to the player) ------------------
    // True if `feet` is inside the shaft bore (XZ within the chute, below the mouth,
    // above the room floor) — the host uses this to know when to hand the player to
    // the scripted fall.
    bool inShaft(const x3::phys::Vec3& feet) const;
    // Begin the drop at `feet` (host calls when the player steps into the mouth).
    void beginFall(const x3::phys::Vec3& feet);
    // Advance the scripted fall one frame from the player's current feet. Returns TRUE
    // while the descent is CONTROLLING the player (the host must teleport the player's
    // feet to controlledFeet()); FALSE once Landed/Idle (the host resumes normal
    // walking). XZ is carried from the player (air-steer within the bore); only Y is
    // scripted (accelerate, then catch).
    bool updateFall(float dt, const x3::phys::Vec3& feet);
    x3::phys::Vec3 controlledFeet() const { return m_feet; }
    Phase phase() const { return m_phase; }
    float fallSpeed() const { return m_vel; }         // current downward speed (m/s)

    // ---- INTERACTIVE PROPS TICK -------------------------------------------------
    // Advance the terminal blink + keypad + door animation + elevator ride. Feed the
    // player feet so the elevator auto-boards/descends and carries the rider. Returns
    // the vertical delta the elevator moved the rider this frame (host adds it to the
    // player's Y). `heal`-style hooks not needed here.
    float tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
               const x3::phys::Vec3& playerFeet);

    // ---- Terminal / keypad / door / elevator access (host routes input) ---------
    HoloTerminal&       computer()       { return m_computer; }   // the lore/code terminal
    HoloTerminal&       keypad()         { return m_keypad; }     // the door keypad
    DoorSystem&         doors()          { return m_doors; }
    ElevatorSystem&     elevator()       { return m_elevator; }
    const DescentFallLayout& layout() const { return m_layout; }

    // The nearest interactable terminal (computer or keypad) within `reach` of `eye`,
    // or null. The host toggles it active on E and routes typed chars to it.
    HoloTerminal* nearestTerminal(const x3::phys::Vec3& eye, float reach = 2.6f);

    // Submit a code straight to the keypad sink (console / self-test). Returns true iff
    // accepted (the door unlocked + began opening).
    bool submitKeypad(const std::string& code);

    // ---- Queries (HUD + self-test) ----
    bool built() const { return m_built; }
    uint32_t doorIndex() const { return m_doorIdx; }
    bool doorLocked() const;
    bool doorOpen() const;     // Opening or Open

    // Draw the elevator visuals hook — none needed (the cab is a plain platform); kept
    // for symmetry with other systems.

    // Headless self-test (--test-descentfall). Builds the descent on a HeadlessDevice +
    // Jolt world and asserts D1-D6:
    //   D1 the fall accelerates (Freefall: speed climbs while Falling);
    //   D2 the CATCH engages in the last ~10 m and DECELERATES the drop;
    //   D3 the player LANDS on the dark-room floor — alive, not below it, not on the
    //      club dance floor (lands well above the club floor Y);
    //   D4 the keypad door starts LOCKED + Closed;
    //   D5 a WRONG code is rejected (stays locked); the CORRECT code opens it;
    //   D6 the elevator rides the last leg down to the club floor.
    static bool runSelfTest();

private:
    DescentFallLayout m_layout{};
    HoloTerminal      m_computer;
    HoloTerminal      m_keypad;
    DoorSystem        m_doors;
    uint32_t          m_doorIdx = kNoLink;
    ElevatorSystem    m_elevator;
    int               m_clubStopIdx = 0;   // the elevator stop that meets the club floor
    x3::audio::IAudioSystem* m_audio = nullptr;

    // Fall state.
    Phase          m_phase = Phase::Idle;
    x3::phys::Vec3 m_feet{};
    float          m_vel = 0.0f;     // downward speed (m/s), positive = down
    float          m_stateTime = 0.0f;

    // Elevator ride bookkeeping.
    bool           m_elevCalled = false;

    bool           m_built = false;
};

} // namespace x3::game
