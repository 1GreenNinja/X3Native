#pragma once
// DESC MECHANICS (W9-1) — the Tier-A gameplay verbs the canonical level data's
// room `desc` fields promise (docs/DESC_MECHANICS_TODO.md). W8-1 shipped the
// VISUAL half (room_dressing desc-gold anchors); this is the MECHANICS half,
// riding the Interactables framework (interactables.h):
//
//   1. Coolant sabotage (F4 "Coolant System")  — console interact -> StoryFlags
//      `f4.coolant_sabotaged` -> The Collective takes x1.5 damage; the room's
//      recipe glow dies (killRoomGlow, host-applied on the edge).
//   2. EMP craft (F4 "Power Junction")         — bench interact -> the EMP item
//      (ItemStore); E-on-demand: ~12 m AoE stun (6 s) on synthetic species.
//   3. Master hack (F5 "Central Control Hub")  — console interact -> `f5.hacked`
//      -> F5's drone-species enemies permanently docile (kills still count).
//   4. Cold Room (F3)                          — entry bark; standing > 30 s ->
//      chill DoT (2 dmg / 2 s) while inside, clears on exit.
//   5. Antidote (F2 Pharmacy + Quarantine)     — component pickups + research
//      note -> bench interact -> `antidote.crafted` + the antidote item; cures
//      the infection DoT (1 dmg / 3 s) that F2/F3 creature hits can apply.
//      Decontamination (F3) ALSO cures (Tier-B #8, free once status exists).
//
// The host wires exactly four seams: build() after CanonPlay::build, onUse()
// in the E-key else-chain, onUseItem() at the chain's end, tick() per frame
// (plus onCue() wrapped around the enemy cue sink for infection). All barks
// ride the existing npcBark path; the "[E] ..." prompt is prompt().
//
// Headless-testable: --test-descmech (runDescMechSelfTest) drives the framework
// AND all five verbs on a HeadlessDevice. Game/slice code only.

#include "interactables.h"
#include "canon_play.h"
#include "cues.h"

#include <string>
#include <vector>

namespace x3::game {

class DescMechanics {
public:
    // Register the five Tier-A mechanics onto the loaded tower. `floor`, `play`
    // and `flags` must outlive this object (host-owned; the self-test owns its
    // own). Rooms absent (single-floor load) register nothing for that verb.
    // If `f4.coolant_sabotaged` is already set (a loaded save), the boss
    // multiplier is re-applied here. Returns true iff at least one point landed.
    bool build(const CanonFloor& floor, CanonPlay& play, StoryFlags& flags);
    bool built() const { return m_floor != nullptr; }

    // Per-frame: cold-room dwell/chill, decontamination cure, pharmacy/
    // quarantine pickup->flag polling, and the status-effect DoT ticks (damage
    // lands on `player`; null-safe for headless geometry ticks).
    void tick(float dt, const x3::phys::Vec3& eye, IDamageSink* player);

    // E-key hook (insert in the host's else-chain): fire the nearest interact
    // point. Returns true iff the E was consumed; `barkOut` gets the bark line.
    bool onUse(const x3::phys::Vec3& eye, std::string* barkOut);

    // E-key FALLBACK (the chain's very end): use a held item. Antidote first
    // (only while infected), else the EMP (discharges only when >= 1 synthetic
    // is in range — the charge is held otherwise, no accidental waste).
    bool onUseItem(const x3::phys::Vec3& eye, std::string* barkOut);

    // Cue hook (wrap around the host's enemy cue sink): a creature-species
    // (Verthani) hit landing on the player on F2/F3 rolls the infection chance.
    void onCue(const GameCue& cue);

    // "[E] ..." HUD prompt for the nearest in-range point ("" when none).
    std::string prompt(const x3::phys::Vec3& eye) const { return m_points.prompt(eye); }

    // Queued bark from tick()/onCue() (entry warnings, infection, decon cure).
    // Pops one line per call ("" when none) — the host routes it to npcBark.
    std::string takeBark();

    // The persistent HUD text tag: held items + active statuses
    // ("EMP READY | ANTIDOTE | INFECTED | FREEZING"), "" when nothing to show.
    std::string hudStatusLine() const;

    // One-shot edge: true exactly once after the coolant console fires — the
    // host kills the Coolant System room's recipe glow (killRoomGlow) on it.
    bool coolantGlowKillPending();
    uint32_t coolantRoom() const { return m_coolantRoom; }

    // Test/diagnostic taps.
    Interactables&       points() { return m_points; }
    ItemStore&           items()  { return m_items; }
    StatusEffects&       status() { return m_status; }
    const StatusEffects& status() const { return m_status; }
    float coldDwell() const { return m_coldDwell; }
    void  setInfectChance(float c) { m_infectChance = c; }   // test: 1.0 = always

    static constexpr float kColdRoomGraceSec = 30.0f;
    static constexpr float kEmpRadius        = 12.0f;
    static constexpr float kEmpStunSecs      = 6.0f;
    static constexpr float kInfectChanceDefault = 0.35f;

    // Item ids (ItemStore keys; the parallel inventory system adopts these).
    static constexpr const char* kItemEmp      = "emp_device";
    static constexpr const char* kItemAntidote = "antidote";

private:
    void queueBark(std::string b);

    Interactables m_points;
    ItemStore     m_items;
    StatusEffects m_status;
    StoryFlags*       m_flags = nullptr;
    CanonPlay*        m_play  = nullptr;
    const CanonFloor* m_floor = nullptr;

    uint32_t m_coolantRoom = kNoRoom, m_coldRoom = kNoRoom, m_deconRoom = kNoRoom;
    uint32_t m_pharmacyRoom = kNoRoom, m_quarantineRoom = kNoRoom;

    float m_coldDwell = 0.0f;
    bool  m_inColdRoom = false;
    bool  m_coolantGlowKill = false;
    float m_infectChance = kInfectChanceDefault;
    uint32_t m_rng = 0x9E3779B9u;
    std::vector<std::string> m_barks;
};

// Kill a room's recipe glow: every CanonLight tagged with `room` drops to a
// faint red emergency ember (the coolant console's "glow dies" statement).
// Returns how many lights were modified. Free function so the self-test can
// drive it on a synthetic light list without a render device.
uint32_t killRoomGlow(std::vector<CanonLight>& lights, uint32_t room);

// Headless self-test (--test-descmech): the Interactables/ItemStore/Status
// framework + all five Tier-A verbs (interact fires, flag sets, boss x1.5
// applies, EMP stun freezes a synth, chill ticks, infection applies + cures,
// decon cures, master hack dociles). Skips-as-PASS without the canonical JSON.
bool runDescMechSelfTest();

} // namespace x3::game
