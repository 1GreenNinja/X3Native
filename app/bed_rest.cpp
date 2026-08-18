// BED REST — Jake's cell hospital-bed interaction. See app/bed_rest.h.
#include "bed_rest.h"

#include "door.h"
#include "headless_device.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace x3::game {

namespace {
// Smoothstep on the linear cursor: zero-slope at both ends (the camera eases
// off the stand and settles onto the pillow instead of snapping). Pure function
// of the dt-integrated cursor — frame-rate independent by construction.
inline float bedEase(float u) {
    u = std::min(std::max(u, 0.0f), 1.0f);
    return u * u * (3.0f - 2.0f * u);
}
// Shortest-arc wrap for the yaw blend.
inline float wrapPi(float a) {
    constexpr float kPi = 3.14159265358979f;
    while (a >  kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
} // namespace

void BedRestSystem::build(const x3::phys::Vec3& lieEye, float lieYaw, float liePitch,
                          uint32_t room, float interactRadius) {
    m_lieEye   = lieEye;
    m_lieYaw   = lieYaw;
    m_liePitch = liePitch;
    m_room     = room;
    m_radius   = interactRadius;
    m_state    = State::Idle;
    m_t        = 0.0f;
    m_lieEdge  = m_riseEdge = false;
    m_built    = true;
    x3::logInfo("[bed] rest point built at (" + std::to_string(lieEye.x) + ", " +
                std::to_string(lieEye.y) + ", " + std::to_string(lieEye.z) + ") room " +
                (room == kBedNoRoom ? std::string("<none>") : std::to_string(room)));
}

bool BedRestSystem::inRange(const x3::phys::Vec3& eye) const {
    if (!m_built) return false;
    const float dx = eye.x - m_lieEye.x, dz = eye.z - m_lieEye.z;
    if (dx * dx + dz * dz > m_radius * m_radius) return false;
    return std::fabs(eye.y - m_lieEye.y) < 2.5f;   // same-storey sanity band
}

std::string BedRestSystem::prompt(const x3::phys::Vec3& eye) const {
    if (!m_built) return {};
    if (m_state == State::Lowering || m_state == State::Lying) return "[E] Get up";
    if (m_state == State::Idle && inRange(eye)) return "[E] Lie down";
    return {};
}

bool BedRestSystem::interact(const x3::phys::Vec3& eye) {
    if (!m_built) return false;
    switch (m_state) {
        case State::Idle:
            if (!inRange(eye)) return false;
            m_state   = State::Lowering;      // cursor keeps its value (0 here)
            m_lieEdge = true;                 // host: closeAndLock + lights out
            x3::logInfo("[bed] lying down — cell door locks, lights out");
            return true;
        case State::Lowering:
        case State::Lying:
            requestRise();
            return true;
        case State::Rising:
            return true;                      // already getting up: E is consumed, no-op
    }
    return false;
}

void BedRestSystem::requestRise() {
    if (m_state != State::Lowering && m_state != State::Lying) return;
    m_state    = State::Rising;               // mid-travel reversal keeps the cursor
    m_riseEdge = true;                        // host: unlock + lights back on
    x3::logInfo("[bed] getting up — door unlocks, lights on");
}

void BedRestSystem::tick(float dt) {
    if (!m_built || dt <= 0.0f) return;
    if (m_state == State::Lowering) {
        m_t += dt / kDownDuration;
        if (m_t >= 1.0f) { m_t = 1.0f; m_state = State::Lying; }
    } else if (m_state == State::Rising) {
        m_t -= dt / kUpDuration;
        if (m_t <= 0.0f) { m_t = 0.0f; m_state = State::Idle; }
    }
}

bool BedRestSystem::camera(float& x, float& y, float& z, float& yaw, float& pitch) const {
    if (!m_built || m_state == State::Idle) return false;
    const float e = bedEase(m_t);
    x     += (m_lieEye.x - x) * e;
    y     += (m_lieEye.y - y) * e;
    z     += (m_lieEye.z - z) * e;
    yaw   += wrapPi(m_lieYaw - yaw) * e;
    pitch += (m_liePitch - pitch) * e;
    return true;
}

// ===========================================================================
// --test-bedrest (B1-B6). The bed loop the gate names: lie -> lights off +
// door locked; rise -> restored. Headless; a REAL DoorSystem door proves the
// lock consumer end-to-end.
// ===========================================================================
namespace {
int b_pass = 0, b_fail = 0;
void bcheck(bool cond, const char* name) {
    if (cond) { ++b_pass; x3::logInfo(std::string("[bed-test] PASS ") + name); }
    else      { ++b_fail; x3::logError(std::string("[bed-test] FAIL ") + name); }
}
} // namespace

bool runBedRestSelfTest() {
    b_pass = b_fail = 0;

    const x3::phys::Vec3 lieEye{ 2.0f, 1.25f, 3.0f };
    const float lieYaw = 1.5707963f, liePitch = 1.25f;

    // ---- B1: build + prompt + reach ----------------------------------------
    {
        BedRestSystem bed;
        bed.build(lieEye, lieYaw, liePitch, /*room*/ 7);
        const x3::phys::Vec3 near1{ 2.5f, 1.6f, 3.4f };
        const x3::phys::Vec3 far1{ 9.0f, 1.6f, 3.0f };
        const bool prompts = bed.built() &&
                             bed.prompt(near1) == "[E] Lie down" &&
                             bed.prompt(far1).empty() &&
                             bed.inRange(near1) && !bed.inRange(far1);
        // Out of reach: the E is NOT consumed and nothing latches.
        const bool refused = !bed.interact(far1) && bed.state() == BedRestSystem::State::Idle &&
                             !bed.tookLieEdge();
        bcheck(prompts && refused, "B1 build + prompt in reach, refused out of reach");
    }

    // ---- B2: THE LOOP — lie: door LOCKED + lights out; rise: restored -------
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        HeadlessRenderDevice dev; Scene scene; DoorSystem doors;
        DoorSpec spec;
        spec.doorwayCenter = x3::phys::Vec3{ 0, 0, 0 };
        spec.halfWidth = 0.8f; spec.height = 2.2f; spec.duration = 1.0f;
        spec.withButton = false;
        buildLevelDoor(scene, doors, dev, *w, spec);
        Door& cellDoor = doors.at(0);
        // The player walked IN through the door and left it open.
        doors.startOpening(cellDoor);
        for (int i = 0; i < 90; ++i) { doors.update(1.0f / 60.0f, scene, *w); w->step(1.0f / 60.0f); }
        const bool doorOpen = cellDoor.state == DoorState::Open;

        BedRestSystem bed;
        bed.build(lieEye, lieYaw, liePitch, 7);
        const x3::phys::Vec3 atBed{ 2.3f, 1.6f, 3.2f };
        const bool consumed = bed.interact(atBed);
        // HOST GLUE (exactly what app_run does on the edge):
        bool lieApplied = false;
        if (bed.tookLieEdge()) { doors.closeAndLock(cellDoor); lieApplied = true; }
        const bool locking = lieApplied && cellDoor.isLocked() &&
                             cellDoor.state == DoorState::Closing &&
                             bed.darkRoom() == 7;                    // lights OUT
        // The lock holds against the E toggle while it seats (the D7 contract).
        const bool sealedEn = !doors.toggle(cellDoor);
        for (int i = 0; i < 120; ++i) {
            bed.tick(1.0f / 60.0f);
            doors.update(1.0f / 60.0f, scene, *w);
            w->step(1.0f / 60.0f);
        }
        const bool downLocked = bed.lying() && cellDoor.state == DoorState::Closed &&
                                cellDoor.isLocked() && bed.darkRoom() == 7;
        // RISE (E again): unlock + lights restored; door STAYS closed.
        bed.interact(atBed);
        bool riseApplied = false;
        if (bed.tookRiseEdge()) { doors.unlock(cellDoor); riseApplied = true; }
        for (int i = 0; i < 90; ++i) bed.tick(1.0f / 60.0f);
        const bool restored = riseApplied && !cellDoor.isLocked() &&
                              cellDoor.state == DoorState::Closed &&
                              bed.state() == BedRestSystem::State::Idle &&
                              bed.darkRoom() == BedRestSystem::kBedNoRoom &&
                              doors.startOpening(cellDoor);          // openable again
        w->shutdown();
        bcheck(doorOpen && consumed && locking && sealedEn && downLocked && restored,
               "B2 lie: door closes+LOCKS, room dark; rise: unlocked, lights back, door usable");
    }

    // ---- B3: the camera blend is dt-scaled (60 Hz vs 144 Hz agree) ----------
    {
        auto blendAt = [&](float dt, float simSeconds) -> float {
            BedRestSystem bed;
            bed.build(lieEye, lieYaw, liePitch, BedRestSystem::kBedNoRoom);
            bed.interact(x3::phys::Vec3{ 2.3f, 1.6f, 3.2f });
            const int steps = (int)std::lround(simSeconds / dt);
            for (int i = 0; i < steps; ++i) bed.tick(dt);
            float x = 2.3f, y = 1.6f, z = 3.2f, yaw = 0.0f, pitch = 0.0f;
            bed.camera(x, y, z, yaw, pitch);
            return y;   // the descending eye height IS the blend readout
        };
        const float y60  = blendAt(1.0f / 60.0f,  0.45f);
        const float y144 = blendAt(1.0f / 144.0f, 0.45f);
        const bool agree = std::fabs(y60 - y144) < 0.01f;
        // ...and the blend is genuinely mid-flight at half the duration.
        const bool midway = y60 < 1.59f && y60 > 1.26f;
        bcheck(agree && midway, "B3 camera blend dt-scaled (60 vs 144 Hz agree mid-flight)");
    }

    // ---- B4: fully down = the authored lying pose ---------------------------
    {
        BedRestSystem bed;
        bed.build(lieEye, lieYaw, liePitch, BedRestSystem::kBedNoRoom);
        bed.interact(x3::phys::Vec3{ 2.3f, 1.6f, 3.2f });
        for (int i = 0; i < 90; ++i) bed.tick(1.0f / 60.0f);
        float x = 2.3f, y = 1.6f, z = 3.2f, yaw = 0.2f, pitch = -0.1f;
        const bool drove = bed.camera(x, y, z, yaw, pitch);
        const bool pose = drove && bed.lying() &&
                          std::fabs(x - lieEye.x) < 1e-3f && std::fabs(y - lieEye.y) < 1e-3f &&
                          std::fabs(z - lieEye.z) < 1e-3f &&
                          std::fabs(yaw - lieYaw) < 1e-3f && std::fabs(pitch - liePitch) < 1e-3f;
        bcheck(pose, "B4 fully down: camera sits at the authored lying pose (staring up)");
    }

    // ---- B5: move input gets you up (the WASD path) -------------------------
    {
        BedRestSystem bed;
        bed.build(lieEye, lieYaw, liePitch, 7);
        bed.interact(x3::phys::Vec3{ 2.3f, 1.6f, 3.2f });
        (void)bed.tookLieEdge();
        for (int i = 0; i < 90; ++i) bed.tick(1.0f / 60.0f);
        bed.requestRise();                       // the host's any-move hook
        const bool rose = bed.tookRiseEdge() && bed.state() == BedRestSystem::State::Rising &&
                          bed.darkRoom() == BedRestSystem::kBedNoRoom;
        for (int i = 0; i < 60; ++i) bed.tick(1.0f / 60.0f);
        float x = 2.3f, y = 1.6f, z = 3.2f;
        float yaw = 0, pitch = 0;
        const bool released = bed.state() == BedRestSystem::State::Idle &&
                              !bed.camera(x, y, z, yaw, pitch);   // camera handed back
        bcheck(rose && released, "B5 move input rises; camera fully released at the top");
    }

    // ---- B6: E mid-lower reverses seamlessly (cursor preserved, no pop) -----
    {
        BedRestSystem bed;
        bed.build(lieEye, lieYaw, liePitch, BedRestSystem::kBedNoRoom);
        const x3::phys::Vec3 atBed{ 2.3f, 1.6f, 3.2f };
        bed.interact(atBed);
        for (int i = 0; i < 20; ++i) bed.tick(1.0f / 60.0f);   // ~1/3 of the way down
        const float cBefore = bed.cursor();
        bed.interact(atBed);                                    // reverse mid-travel
        const bool kept = bed.state() == BedRestSystem::State::Rising &&
                          std::fabs(bed.cursor() - cBefore) < 1e-6f;
        bcheck(kept, "B6 mid-travel E reverses on the SAME cursor (no pop)");
    }

    x3::logInfo(std::string("[bed-test] ") + std::to_string(b_pass) + " passed, " +
                std::to_string(b_fail) + " failed");
    return b_fail == 0;
}

} // namespace x3::game
