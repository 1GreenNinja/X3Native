#pragma once
// PlayerSimStep — the ONE deterministic per-tick player movement step.
// Spec: specs/NETCODE-architecture.spec.md §6.1 / §8 ("one function, two callers").
//
// Clean-room: built from X3Native's OWN net types (NetTypes.h NetCommand / Rep*) +
// PUBLIC references (Valve "Source Multiplayer Networking" prediction, Fiedler
// "Networked Physics", Overwatch GDC 2017 command-frame). NO third-party types.
//
// The §0 principle made concrete: the SAME deterministic player integrator is run
//   (a) by the SERVER to advance authoritative state for a drained command, and
//   (b) by the CLIENT to PREDICT the local player immediately AND to RE-SIMULATE
//       (rollback/resim, §6.3) unacknowledged commands after a reconciliation.
// Both callers step at exactly kSimDt with byte-identical math, which is what makes
// "predict then replay arrives at the same state the server computed" hold to the
// last bit (single-machine fixed-step determinism, §3.2). Header-only so prediction
// re-sim and the authoritative apply share BYTE-IDENTICAL logic with no link seam.
//
// This is intentionally self-contained planar movement (clean-room stand-in for the
// real Player::update path) so prediction/reconciliation can be developed + tested
// with NO render/physics deps — the integrator is swapped for the real character
// controller later WITHOUT changing the predict/reconcile machinery around it.

#include "engine/net/NetTypes.h"
#include "engine/net/SimClock.h"   // kSimDt

#include <cmath>

namespace x3::net {

// State byte bits packed into RepHealth::flags by the deterministic step (so the
// predicted/authoritative "state byte" can be compared). Kept in sync with the
// Phase 0b values used by --test-netsync (NetworkSystem.cpp NetState).
enum PlayerStateBit : uint8_t {
    PlayerState_Moving    = 1u << 0,   // |move axes| above the deadzone this tick
    PlayerState_Sprinting = 1u << 1,   // sprint button held
    PlayerState_Firing    = 1u << 2,   // fire button held
};

// The authoritative/predicted player state advanced one tick at a time. POD so a
// predicted copy is trivially snapshot/replay-able. Mirrors the Rep* blocks the
// server writes (RepTransform + RepVelocity + the RepHealth state byte) so a
// predicted state and an authoritative snapshot compare field-for-field.
struct PlayerSimState {
    RepTransform xf{};      // position + facing quaternion (x,y,z,w)
    RepVelocity  vel{};     // linear velocity (planar)
    uint8_t      state = 0; // PlayerStateBit flags resolved from buttons + motion
};

// Movement constants — deterministic scalars shared by predict + authority. Kept
// IDENTICAL to the Phase 0b serverApplyCommand values so a snapshot produced by the
// server reconciles against client prediction with zero steady-state correction.
constexpr float kPlayerBaseSpeed   = 4.0f;   // m/s
constexpr float kPlayerSprintScale = 1.8f;

// Advance `s` by exactly one fixed sim tick under command `cmd`. Pure function of
// (s, cmd): same inputs => same output bits, on this build/machine (§3.2). This is
// the single integrator the server applies authoritatively AND the client predicts
// + replays with. No allocation, no globals, no wall-clock — only kSimDt.
inline void stepPlayer(PlayerSimState& s, const NetCommand& cmd) {
    const bool  sprint = (cmd.buttons & NetBtn_Sprint) != 0;
    const float speed  = kPlayerBaseSpeed * (sprint ? kPlayerSprintScale : 1.0f);

    // Yaw-derived planar basis (matches Phase 0b serverApplyCommand exactly):
    //   forward = (sin yaw, 0, -cos yaw),  right = (cos yaw, 0, sin yaw)
    const float sn = std::sin(cmd.yaw);
    const float cs = std::cos(cmd.yaw);
    const float fwd[3]   = {  sn, 0.0f, -cs };
    const float right[3] = {  cs, 0.0f,  sn };

    const float vx = (fwd[0] * cmd.moveFwd + right[0] * cmd.moveStrafe) * speed;
    const float vz = (fwd[2] * cmd.moveFwd + right[2] * cmd.moveStrafe) * speed;

    s.xf.pos[0] += vx * kSimDt;
    s.xf.pos[2] += vz * kSimDt;

    // Face the look yaw (quaternion about +Y, x,y,z,w order).
    const float half = cmd.yaw * 0.5f;
    s.xf.rotQuat[0] = 0.0f;
    s.xf.rotQuat[1] = std::sin(half);
    s.xf.rotQuat[2] = 0.0f;
    s.xf.rotQuat[3] = std::cos(half);

    s.vel.lin[0] = vx; s.vel.lin[1] = 0.0f; s.vel.lin[2] = vz;
    s.vel.ang[0] = s.vel.ang[1] = s.vel.ang[2] = 0.0f;

    const float moveMag2 = cmd.moveFwd * cmd.moveFwd + cmd.moveStrafe * cmd.moveStrafe;
    uint8_t st = 0;
    if (moveMag2 > 1e-6f)           st |= PlayerState_Moving;
    if (sprint)                     st |= PlayerState_Sprinting;
    if (cmd.buttons & NetBtn_Fire)  st |= PlayerState_Firing;
    s.state = st;
}

} // namespace x3::net
