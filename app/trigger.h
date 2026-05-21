#pragma once
// Trigger volumes (Level 1 / §6.3).
//
// Game/slice code only — engine/ stays pure. A box AABB trigger that fires ONCE
// the first frame the player's position enters it. The spec calls out two valid
// implementations: Jolt overlap callbacks (setTriggerCallback) or a per-frame
// AABB test on the player position. We take the per-frame point-in-box test:
// it is simple, deterministic, allocation-free, and trivially testable headless
// (no physics step required) — exactly what the §6 thinnest build wants.
//
// Each trigger carries a small integer `id` the host switches on when the trigger
// fires (strength-discovery, boss-arena entry, elevator win, ...). The system
// reports the fired trigger ids from update() so the host can react in one place.

#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// A world-space axis-aligned box trigger. `min`/`max` are the box corners. Fires
// once (latched) the first time a tested point falls inside [min,max]. `id` is a
// host-defined tag returned by TriggerSystem::update() so the caller can dispatch.
struct TriggerVolume {
    x3::phys::Vec3 min{};
    x3::phys::Vec3 max{};
    uint32_t       id      = 0;      // host-defined event tag
    bool           fired   = false;  // latched: true after the first entry
    bool           enabled = true;   // host can arm/disarm without removing it
};

// True iff point p is inside the closed AABB [min,max]. Pure; testable.
bool pointInBox(const x3::phys::Vec3& p, const x3::phys::Vec3& min, const x3::phys::Vec3& max);

// Registry of trigger volumes. The host adds boxes, then calls update(point)
// every frame with the player position; update() returns the ids of any triggers
// that fired THIS call (each at most once over its lifetime).
class TriggerSystem {
public:
    // Add a trigger volume. `id` is the event tag; `enabled` lets the host stage a
    // trigger that should only become live later (e.g. the elevator win, which is
    // gated on the boss being dead). Returns the trigger's index.
    uint32_t add(const x3::phys::Vec3& min, const x3::phys::Vec3& max,
                 uint32_t id, bool enabled = true);

    uint32_t count() const { return (uint32_t)m_vols.size(); }
    TriggerVolume&       at(uint32_t i)       { return m_vols[i]; }
    const TriggerVolume& at(uint32_t i) const { return m_vols[i]; }

    // Find the first trigger with the given event id, or nullptr.
    TriggerVolume*       findById(uint32_t id);
    const TriggerVolume* findById(uint32_t id) const;

    // Enable/disable a trigger by event id (no-op if not found). Used to arm the
    // win trigger only after the boss dies.
    void setEnabled(uint32_t id, bool enabled);

    // Test `point` (the player position) against every enabled, un-fired trigger.
    // Latch + collect the ids of any that fire this frame, in registration order.
    // Returns those ids (empty if none fired). A fired trigger never fires again.
    std::vector<uint32_t> update(const x3::phys::Vec3& point);

private:
    std::vector<TriggerVolume> m_vols;
};

// Headless self-test (folded into --test-level1, but standalone-callable). Drives
// pointInBox + a TriggerSystem with synthetic positions (enter once, re-enter is
// a no-op, disabled never fires, enabling later fires). No physics/device.
bool runTriggerSelfTest();

} // namespace x3::game
