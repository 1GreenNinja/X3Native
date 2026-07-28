// THE DESCENT FALL — interactive layer. See descent_fall.h.
//
// Clean-room: built from the Scene / HoloTerminal / Door / Elevator systems + the
// engine interfaces only.
#include "descent_fall.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {
constexpr float kPi     = 3.14159265f;
constexpr float kHalfPi = 1.57079633f;

// --- Scripted fall tuning (adapted from ElevState::Freefall "accelerate down hard").
constexpr float kFallAccel   = 22.0f;   // m/s^2 downward acceleration
constexpr float kFallMaxV    = 58.0f;   // terminal speed clamp (m/s) — a fast, dramatic drop
// The CATCH (last ~10 m): an updraft/grav-damp. The desired speed scales with the
// remaining height so the drop eases to a soft touchdown; the floor clamp guarantees
// the player never punches through (safe landing, never dead).
constexpr float kCatchBase   = 2.0f;    // residual settle speed near the floor (m/s)
constexpr float kCatchGain   = 3.2f;    // desired speed added per metre remaining
} // namespace

void DescentFall::build(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld& physics, const DescentFallLayout& layout,
                        std::string_view /*modelDir*/, x3::audio::IAudioSystem* audio) {
    m_layout = layout;
    m_audio  = audio;

    // ================================================================
    // 1) THE COMPUTER TERMINAL — a glowing HoloTerminal on the dark room's +Z (north)
    //    wall, facing the landing spot. Its readout carries the lore + the door code.
    // ================================================================
    const float rMaxZ = layout.roomCz + layout.roomHalfZ;
    const x3::phys::Vec3 compPos{ layout.roomCx, layout.roomFloorY + 1.4f, rMaxZ - 0.35f };
    m_computer.setLayout(HoloTerminal::Layout::Readout);
    m_computer.build(scene, device, compPos, /*yaw*/0.0f, 1.6f, 1.0f, layout.roomCeilY);
    // Solid glass plate (you can't walk through glass).
    physics.addBox(x3::phys::Vec3{ 0.85f, 0.55f, 0.06f }, compPos, 0.0f, x3::phys::Layer::Static);
    m_computer.setLines({
        std::string("SUB-STRATA ACCESS TERMINAL  //  LEVEL -") +
            std::to_string((int)std::lround(-layout.roomFloorY)) + "m",
        "> impact logged. subject intact.",
        "> you fell the whole shaft. the DEEP is below.",
        "> CLUB 1127 lies past this door.",
        "> DOOR OVERRIDE CODE:  " + std::string(kDescentDoorCode),
        "> enter it on the keypad. the hall ends at the lift.",
    });

    // ================================================================
    // 2) THE KEYPAD DOOR — a code-locked sliding door in the room's -X (west) wall
    //    (leads onto the hall) + a keypad HoloTerminal beside it whose submit sink
    //    unlock+opens the door on the correct code.
    // ================================================================
    {
        DoorSpec d;
        d.doorwayCenter = x3::phys::Vec3{ layout.doorX, layout.doorY, layout.doorZ };
        d.axis        = DoorAxis::AlongZ;         // the -X wall runs along Z (plane X=const)
        d.halfWidth   = layout.doorHalfW;
        d.height      = 2.6f;
        d.thickness   = 0.12f;
        d.duration    = 1.4f;
        d.withButton  = false;                    // opened by the keypad code, not a wall button
        d.locked      = true;
        d.code        = std::atoi(kDescentDoorCode);
        d.tint[0]=0.24f; d.tint[1]=0.27f; d.tint[2]=0.34f; d.tint[3]=1.0f;   // dark steel
        m_doorIdx = buildLevelDoor(scene, m_doors, device, physics, d);
    }

    // The keypad terminal, mounted on the -X wall just beside the door, facing +X
    // (into the room / toward the landing spot).
    const x3::phys::Vec3 padPos{ layout.doorX + 0.30f, layout.roomFloorY + 1.25f,
                                 layout.doorZ + 1.7f };
    m_keypad.build(scene, device, padPos, /*yaw*/kHalfPi, 0.8f, 0.6f, layout.roomCeilY);
    physics.addBox(x3::phys::Vec3{ 0.06f, 0.34f, 0.44f }, padPos, 0.0f, x3::phys::Layer::Static);
    m_keypad.setLines({ "DOOR KEYPAD", "> enter override code", "> [ #### ] -> HALL" });
    m_keypad.setSubmitSink([this](const std::string& v) -> bool {
        if (v != kDescentDoorCode) {                       // reject any other code
            if (m_audio) {
                const x3::phys::Vec3 a = m_keypad.anchor();
                (void)a;   // (a reject buzz would play here if a cue were wired)
            }
            return false;
        }
        if (m_doorIdx == kNoLink || m_doorIdx >= m_doors.count()) return false;
        Door& d = m_doors.at(m_doorIdx);
        if (d.state == DoorState::Opening || d.state == DoorState::Open) return true;  // idempotent
        const bool opened = m_doors.unlockAndOpen(d);
        if (opened) x3::logInfo("[descent] keypad code accepted — dark-room door opening onto the hall");
        return opened;
    });

    // ================================================================
    // 3) THE ELEVATOR — the last leg down into the club. A plain industrial lift
    //    platform in the alcove at the hall's end, two stops (club floor / hall floor),
    //    parked at the TOP (where the player arrives from the hall).
    // ================================================================
    m_elevator.build(scene, device, physics, layout.elevX, layout.elevZ,
                     /*cabHalf*/2.2f, 0.22f, 2.2f,
                     std::vector<float>{ layout.elevBotY, layout.elevTopY }, /*startStop*/1);
    m_elevator.setSpeed(4.0f);

    m_built = true;
    x3::logInfo("[descent] built: computer terminal + code-locked keypad door (code " +
                std::string(kDescentDoorCode) + ") + elevator (last leg to the club at Y=" +
                std::to_string((int)std::lround(layout.elevBotY)) + ")");
}

// ---------------------------------------------------------------------------
// THE FALL (scripted).
// ---------------------------------------------------------------------------
bool DescentFall::inShaft(const x3::phys::Vec3& feet) const {
    const float hw = m_layout.shaftHalfW;
    return std::fabs(feet.x - m_layout.shaftX) < hw &&
           std::fabs(feet.z - m_layout.shaftZ) < hw &&
           feet.y < m_layout.mouthY + 1.0f &&
           feet.y > m_layout.roomFloorY + 0.15f;
}

void DescentFall::beginFall(const x3::phys::Vec3& feet) {
    m_phase = Phase::Falling;
    m_feet = feet;
    m_vel = 0.0f;
    m_stateTime = 0.0f;
    x3::logInfo("[descent] FREEFALL — down the shaft");
}

bool DescentFall::updateFall(float dt, const x3::phys::Vec3& feet) {
    if (m_phase != Phase::Falling && m_phase != Phase::Catching) return false;
    m_stateTime += dt;

    // Carry the player's XZ (they can air-steer within the bore), clamped inside the
    // chute so the scripted teleport never clips a wall.
    const float m = m_layout.shaftHalfW - 0.4f;
    m_feet.x = std::clamp(feet.x, m_layout.shaftX - m, m_layout.shaftX + m);
    m_feet.z = std::clamp(feet.z, m_layout.shaftZ - m, m_layout.shaftZ + m);

    if (m_phase == Phase::Falling) {
        // Accelerate down hard (ElevState::Freefall parity).
        m_vel = std::min(m_vel + kFallAccel * dt, kFallMaxV);
        m_feet.y -= m_vel * dt;
        if (m_feet.y <= m_layout.catchTopY) {
            m_phase = Phase::Catching;
            m_stateTime = 0.0f;
            x3::logInfo("[descent] CATCH — updraft decelerating the drop");
        }
        return true;
    }

    // Catching: the updraft/grav-damp. Desired speed scales with remaining height so
    // the drop eases to a soft touchdown; the floor clamp guarantees a safe landing.
    const float remaining = m_feet.y - m_layout.roomFloorY;      // > 0
    const float desired = kCatchBase + kCatchGain * std::max(0.0f, remaining);
    m_vel = std::min(m_vel, desired);                            // only ever slow down
    m_feet.y -= m_vel * dt;
    if (m_feet.y <= m_layout.roomFloorY) {
        m_feet.y = m_layout.roomFloorY;
        m_vel = 0.0f;
        m_phase = Phase::Landed;
        x3::logInfo("[descent] LANDED — safe on the dark-room floor");
        return true;   // one last controlled frame to seat the feet on the floor
    }
    return true;
}

// ---------------------------------------------------------------------------
// INTERACTIVE PROPS TICK.
// ---------------------------------------------------------------------------
float DescentFall::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                        const x3::phys::Vec3& playerFeet) {
    m_computer.update(dt);
    m_keypad.update(dt);
    m_doors.update(dt, scene, physics);

    // Elevator: when the player boards at the top and it's idle, auto-descend into the
    // club (the last leg). It rides back up on its own wrap when re-called.
    const bool riding = m_elevator.playerRiding(playerFeet);
    if (riding && !m_elevCalled && !m_elevator.moving() &&
        m_elevator.nearestStopTo(playerFeet.y) == 1) {
        m_elevator.callTo(0);                 // descend to the club floor
        m_elevCalled = true;
        x3::logInfo("[descent] elevator descending the last leg into Club 1127");
    }
    if (!riding) m_elevCalled = false;        // re-arm once they step off
    const float dY = m_elevator.update(dt, scene, physics);
    return riding ? dY : 0.0f;
}

HoloTerminal* DescentFall::nearestTerminal(const x3::phys::Vec3& eye, float reach) {
    HoloTerminal* best = nullptr;
    float bd = reach * reach;
    auto consider = [&](HoloTerminal& t) {
        const x3::phys::Vec3 a = t.anchor();
        const float dx = a.x - eye.x, dy = a.y - eye.y, dz = a.z - eye.z;
        const float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < bd) { bd = d2; best = &t; }
    };
    consider(m_computer);
    consider(m_keypad);
    return best;
}

bool DescentFall::submitKeypad(const std::string& code) {
    if (!m_built) return false;
    m_keypad.setActive(true);
    m_keypad.clearInput();
    for (char c : code) m_keypad.pushChar(c);
    return m_keypad.submit();
}

bool DescentFall::doorLocked() const {
    if (m_doorIdx == kNoLink || m_doorIdx >= const_cast<DoorSystem&>(m_doors).count()) return false;
    return const_cast<DoorSystem&>(m_doors).at(m_doorIdx).locked;
}
bool DescentFall::doorOpen() const {
    if (m_doorIdx == kNoLink || m_doorIdx >= const_cast<DoorSystem&>(m_doors).count()) return false;
    const DoorState s = const_cast<DoorSystem&>(m_doors).at(m_doorIdx).state;
    return s == DoorState::Opening || s == DoorState::Open;
}

// ---------------------------------------------------------------------------
// Headless self-test (--test-descentfall).
// ---------------------------------------------------------------------------
namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) { ++g_pass; x3::logInfo(std::string("[descent-test] PASS ") + what); }
    else    { ++g_fail; x3::logError(std::string("[descent-test] FAIL ") + what); }
}
} // namespace

bool DescentFall::runSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessRenderDevice device;
    Scene scene;
    DescentFall descent;

    // A representative layout (club floor at Y=-800; dark room 12 m above it).
    DescentFallLayout L;
    L.shaftX = 37.0f; L.shaftZ = 4.0f; L.shaftHalfW = 4.0f;
    L.mouthY = -3.0f;
    L.roomCx = 37.0f; L.roomCz = 4.0f;
    L.roomFloorY = -788.0f; L.roomCeilY = -782.0f;
    L.roomHalfX = 6.0f; L.roomHalfZ = 6.0f;
    L.catchTopY = L.roomFloorY + 10.0f;    // -778
    L.doorX = 31.0f; L.doorZ = 4.0f; L.doorY = L.roomFloorY; L.doorHalfW = 1.1f;
    L.elevX = 23.24f; L.elevZ = 4.0f; L.elevTopY = L.roomFloorY; L.elevBotY = -800.0f;
    L.clubDoorX = 15.24f; L.clubDoorZ = 4.0f;
    const float clubFloorY = L.elevBotY;

    descent.build(scene, device, *physics, L, "", nullptr);
    check(descent.built(), "D0 descent interactive layer builds (terminal + keypad door + elevator)");

    // ---- THE FALL: accelerate -> catch -> land safely. ----
    descent.beginFall(x3::phys::Vec3{ L.shaftX, L.mouthY, L.shaftZ });
    float peakV = 0.0f, vAtCatch = -1.0f;
    bool sawFalling = false, sawCatching = false;
    const float dt = 1.0f / 60.0f;
    int guard = 0;
    while (descent.phase() != Phase::Landed && guard++ < 40000) {
        const x3::phys::Vec3 f = descent.controlledFeet();
        descent.updateFall(dt, f);
        if (descent.phase() == Phase::Falling) { sawFalling = true; peakV = std::max(peakV, descent.fallSpeed()); }
        if (descent.phase() == Phase::Catching && vAtCatch < 0.0f) { sawCatching = true; vAtCatch = descent.fallSpeed(); }
    }
    const x3::phys::Vec3 land = descent.controlledFeet();

    check(sawFalling && peakV > 25.0f,
          "D1 the fall ACCELERATES (Freefall: peak speed climbs past 25 m/s)");
    check(sawCatching && descent.fallSpeed() < peakV,
          "D2 the CATCH engages in the last ~10 m and DECELERATES the drop");
    const bool onFloor = land.y >= L.roomFloorY - 0.001f && land.y <= L.roomFloorY + 0.05f;
    const bool notDanceFloor = land.y > clubFloorY + 5.0f;   // well above the club dance floor
    check(descent.phase() == Phase::Landed && onFloor && notDanceFloor,
          "D3 the player LANDS on the dark-room floor — alive, above the club dance floor");

    // ---- THE KEYPAD DOOR: locked -> wrong rejected -> right opens. ----
    check(descent.doorLocked() && !descent.doorOpen(),
          "D4 the keypad door starts LOCKED + Closed");
    {
        const bool bad = descent.submitKeypad("9999");
        check(!bad && descent.doorLocked() && !descent.doorOpen(),
              "D5a a WRONG code is rejected — the door stays locked");
    }
    {
        const bool ok = descent.submitKeypad(kDescentDoorCode);
        check(ok && !descent.doorLocked() && descent.doorOpen(),
              "D5b the CORRECT code (1127) unlocks + opens the door onto the hall");
    }

    // ---- THE ELEVATOR: rides the last leg down to the club floor. ----
    {
        descent.elevator().callTo(0);
        int eguard = 0;
        while (descent.elevator().moving() && eguard++ < 4000)
            descent.elevator().update(dt, scene, *physics);
        const float cabTop = descent.elevator().cabTopY();
        // Cab-center at the bottom stop == club floor; its top ~0.22 above.
        const bool downAtClub = std::fabs(descent.elevator().cabCenter().y - clubFloorY) < 0.05f;
        check(downAtClub && cabTop < L.roomFloorY,
              "D6 the elevator rides the last leg DOWN to the club floor (Y=-800)");
    }

    physics->shutdown();
    x3::logInfo("descentfall: " + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
