#pragma once
// BED REST — the Jake's-cell hospital-bed interaction (doors-pass, part 2).
//
// Tim's spec, verbatim: "when you lie down on the bed ... the lights should go
// off an the door lock." The mechanical loop, nothing more:
//
//   [E] at the cot  -> the camera eases into a lying pose on the mattress, the
//                      cell door CLOSES + LOCKS (DoorSystem::closeAndLock — the
//                      first consumer of the Door lock API) and the cell's
//                      room lights go OFF (the host excludes the cell room from
//                      the canon light feed while resting).
//   [E] again / any move input -> the camera eases back up, the door UNLOCKS
//                      (stays closed) and the lights come back.
//
// v1 is CAMERA-ONLY by design (no lying animation exists): the player capsule
// never moves — the camera blends from wherever the live camera is toward the
// authored lying pose, so release is seamless. All motion is a dt-integrated
// cursor through smoothstep (repo HARD RULE: never per-frame factors).
//
// The class owns STATE + EDGES only (headless-testable, no render/audio deps);
// the host applies the effects on the latched edges. Does sleeping advance
// time / trigger story? Open design question — deliberately not implemented.
//
// Game/slice code only — engine/ stays pure. --test-bedrest drives it headless.

#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>

namespace x3::game {

class BedRestSystem {
public:
    // kNoRoom's value (level_loader.h) without the include: "no room" sentinel.
    static constexpr uint32_t kBedNoRoom = 0xFFFFFFFFu;

    // One bed. `lieEye` = the world point the lying EYE settles at (just above
    // the pillow); `lieYaw` = the lying look yaw (toward the bed's foot);
    // `liePitch` = the upward tilt (radians; ~1.25 = staring at the ceiling).
    // `room` = the canon room whose lights die while resting (kBedNoRoom = none).
    void build(const x3::phys::Vec3& lieEye, float lieYaw, float liePitch,
               uint32_t room, float interactRadius = 2.0f);
    bool built() const { return m_built; }

    enum class State : uint32_t { Idle = 0, Lowering = 1, Lying = 2, Rising = 3 };
    State state()   const { return m_state; }
    bool  active()  const { return m_state != State::Idle; }
    bool  lying()   const { return m_state == State::Lying; }
    // Down or on the way down: the effects (lock + lights-out) hold across both.
    bool  resting() const { return m_state == State::Lowering || m_state == State::Lying; }

    // In interact reach of the cot (XZ radius + a sane Y band). False when unbuilt.
    bool inRange(const x3::phys::Vec3& eye) const;

    // HUD prompt: "[E] Lie down" in reach while Idle; "[E] Get up" while down;
    // "" otherwise. Rides the host's existing bark/prompt line (caller-drawn).
    std::string prompt(const x3::phys::Vec3& eye) const;

    // The E press. Idle + in reach -> Lowering (fires the lie edge). Lowering /
    // Lying -> Rising (fires the rise edge; mid-travel reversal keeps the
    // cursor, so there is no pop). Returns true iff the E was consumed.
    bool interact(const x3::phys::Vec3& eye);

    // The move-input path: any WASD/jump while down gets the player up (same
    // edge as the E path). No-op unless Lowering/Lying.
    void requestRise();

    // Advance the blend cursor (dt-scaled). Lowering settles into Lying;
    // Rising settles into Idle.
    void tick(float dt);

    // Camera blend: mixes the LIVE pose (in/out params) toward the lying pose
    // by the eased cursor — identity at cursor 0, the lying pose at 1. Yaw
    // takes the shortest arc. Returns false (untouched params) while Idle.
    bool camera(float& x, float& y, float& z, float& yaw, float& pitch) const;

    // ---- EFFECT EDGES (latched, read-once — the host applies the effects):
    // lie  = the rest began  -> closeAndLock the cell door(s), lights out.
    // rise = the rise began  -> unlock the door(s), lights back on.
    bool tookLieEdge()  { const bool f = m_lieEdge;  m_lieEdge  = false; return f; }
    bool tookRiseEdge() { const bool f = m_riseEdge; m_riseEdge = false; return f; }

    // The room whose lights are OFF right now (kBedNoRoom unless resting).
    // Callers must treat kBedNoRoom as "exclude nothing" (it aliases kNoRoom,
    // which un-roomed range-fed lights also carry).
    uint32_t darkRoom() const { return resting() ? m_room : kBedNoRoom; }

    // Blend cursor [0..1] (diagnostics + the self-test's dt assertions).
    float cursor() const { return m_t; }

    static constexpr float kDownDuration = 0.9f;   // s, standing -> lying
    static constexpr float kUpDuration   = 0.6f;   // s, lying -> standing

private:
    bool           m_built = false;
    State          m_state = State::Idle;
    float          m_t = 0.0f;             // blend cursor 0 = up, 1 = lying
    x3::phys::Vec3 m_lieEye{};
    float          m_lieYaw = 0.0f, m_liePitch = 1.25f;
    float          m_radius = 2.0f;
    uint32_t       m_room = kBedNoRoom;
    bool           m_lieEdge = false, m_riseEdge = false;
};

// Headless self-test (--test-bedrest). Asserts the mechanical loop the spec
// names — lie: door locked (via a REAL DoorSystem door + closeAndLock) + the
// dark-room latch; rise: unlocked + lights restored — plus the camera blend's
// dt-independence (60 Hz vs 144 Hz integrations agree), the lying pose, the
// seamless mid-travel reversal, and the move-input rise path. Logs PASS/FAIL
// B#, returns true iff all pass. No window / Vulkan.
bool runBedRestSelfTest();

} // namespace x3::game
