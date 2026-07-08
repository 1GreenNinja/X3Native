#pragma once
// INTERACTABLES — the generic use-key (E) interact framework + the two tiny
// substrate pieces the desc-field mechanics ride (W9-1, docs/DESC_MECHANICS_TODO.md).
//
// The host's E-key path already handles doors / rescue / terminals / elevators as
// bespoke branches. This adds the GENERIC layer BESIDE them (it does not rebuild
// them): registered interact points (position, radius, prompt text, StoryFlags
// gate, one-shot/repeat, callback), polled from the same rising-edge E handler,
// with the "[E] ..." HUD prompt riding the existing bark text path (no new UI).
//
// Also here, because every Tier-A mechanic needs them:
//   * ItemStore     — a minimal {id,count} carryable store (EMP / antidote). A
//                     full inventory/backpack system is being built in parallel
//                     (app/inventory.*, not this branch); this stays a deliberate
//                     minimal interface so that system can absorb it with a type
//                     swap, not a rewrite. Do NOT grow it.
//   * StatusEffects — damage-over-time ticker on the player (chill / infection),
//                     each with a source tag; drives IDamageSink::takeDamage so
//                     the existing pain cue + HUD flash fire for free.
//
// MP-friendly / headless-testable style: pure state + tick, no render/audio deps.
// --test-descmech (desc_mechanics.h) drives all of it headless.
//
// Game/slice code only — engine/ stays pure.

#include "story_ops.h"                       // StoryFlags
#include "player.h"                          // IDamageSink
#include "engine/physics/IPhysicsWorld.h"    // x3::phys::Vec3

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// ===========================================================================
// ItemStore — the minimal carryable store ({id, count} rows).
// ===========================================================================
struct ItemStore {
    struct Row { std::string id; int count = 0; };
    std::vector<Row> rows;

    void add(std::string_view id, int n = 1);
    // Count held for `id` (0 when absent).
    int  count(std::string_view id) const;
    bool has(std::string_view id) const { return count(id) > 0; }
    // Remove up to n; returns true iff n were actually held and consumed.
    bool consume(std::string_view id, int n = 1);
    bool empty() const;
};

// ===========================================================================
// Interactables — registered use-key points.
// ===========================================================================
struct InteractPoint {
    std::string    id;            // stable key ("coolant_console")
    x3::phys::Vec3 pos{};         // world anchor
    float          radius = 2.5f; // interact reach (m, planar-ish sphere)
    std::string    prompt;        // HUD line while in range ("[E] SABOTAGE ...")
    // Gate: every listed flag must be set on the StoryFlags world before the
    // point fires. While gated, `missingBark` (if any) is surfaced instead.
    std::vector<std::string> requiresFlags;
    std::string    missingBark;
    bool           oneShot = true;
    bool           used    = false;
    // Fired on a successful use. Returns the bark to surface (may be empty).
    std::function<std::string(StoryFlags&)> onUse;
};

class Interactables {
public:
    // Register a point; returns its index.
    uint32_t add(InteractPoint p);

    // Nearest un-consumed point whose radius contains `eye` (nullptr if none).
    // Gated points ARE returned (their prompt/missingBark should still show).
    const InteractPoint* nearest(const x3::phys::Vec3& eye) const;

    // The host's E-key hook. Fires the nearest in-range point: if gated and the
    // gate fails, surfaces missingBark; else runs onUse, marks one-shots used.
    // Returns true iff the E was consumed (a point was in range at all).
    // `barkOut` (optional) receives the line to surface on the bark path.
    bool onUse(const x3::phys::Vec3& eye, StoryFlags& flags, std::string* barkOut);

    // Per-frame HUD prompt: the nearest in-range point's prompt ("" when none).
    std::string prompt(const x3::phys::Vec3& eye) const;

    InteractPoint*       find(std::string_view id);
    const InteractPoint* find(std::string_view id) const;
    uint32_t count() const { return (uint32_t)m_points.size(); }
    const std::vector<InteractPoint>& points() const { return m_points; }

private:
    std::vector<InteractPoint> m_points;
};

// ===========================================================================
// StatusEffects — DoT ticker on the player (chill / infection).
// ===========================================================================
// Chill:     2 damage every 2 s while active (Cold Room past its 30 s grace);
//            cleared the frame the player leaves the room (host-driven level).
// Infection: 1 damage every 3 s, persists until cured (antidote / Decon room).
// Damage lands through IDamageSink::takeDamage — the player's pain cue + HUD
// damage flash fire exactly as a monster hit does (source tag in the log).
class StatusEffects {
public:
    void setChill(bool on);                  // level-driven (in/out of the room)
    bool chillActive() const { return m_chill; }

    void infect();                           // idempotent while already infected
    void cureInfection();
    bool infected() const { return m_infected; }

    // Advance the tickers; applies due damage to `player` (null-safe: state
    // still advances so headless geometry ticks behave, damage is just skipped).
    void tick(float dt, IDamageSink* player);

    // Diagnostics / self-test taps.
    int chillDamageDealt()     const { return m_chillDealt; }
    int infectionDamageDealt() const { return m_infectDealt; }

    static constexpr float kChillPeriod    = 2.0f;  // s between chill ticks
    static constexpr int   kChillDamage    = 2;
    static constexpr float kInfectPeriod   = 3.0f;  // s between infection ticks
    static constexpr int   kInfectDamage   = 1;

private:
    bool  m_chill = false, m_infected = false;
    float m_chillT = 0.0f, m_infectT = 0.0f;   // time until next tick
    int   m_chillDealt = 0, m_infectDealt = 0;
};

} // namespace x3::game
