// app/space/ship_repair.h
//
// S7 — the in-transit REPAIR gameplay. While the player walks the ship interior
// (S5, app/space/ship_interior.*) — typically during a wormhole transit — damaged
// systems present as wall PANELS. The transit loop: approach a panel -> press
// interact to OPEN it -> connect its wires (the micro-task) -> the panel ramps to
// REPAIRED. When every panel is repaired the ship's systems are restored.
//
// This file is PURE LOGIC + DATA: a panel state machine driven by the player's
// position, the interact key, and a per-wire connect call. It owns NO render or
// physics resources (so it is trivially headless-testable, --test-ship-repair). The
// showcase (`--world ship-repair`, in app/main.cpp) builds the visual wires/panels
// from this model — green line segments that light as wires connect — and DRIVES
// this state machine; it never lives in here.
//
// Game/slice code only — engine/ stays pure. Mirrors the existing E-key interact
// pattern (range check + rising edge) used by `--world npc` / `--world destruct`.
#pragma once

#include <cstdint>
#include <vector>

namespace x3::space {

// One panel's lifecycle. Sealed (damaged, shut) -> Open (player opened it, wiring
// exposed) -> Repairing (all wires connected, progress ramping) -> Repaired (done).
enum class PanelState { Sealed, Open, Repairing, Repaired };

// A single damaged access panel on an interior wall.
struct RepairPanel {
    float pos[3];            // world-space center of the panel face
    float yaw;               // facing (radians) — which wall it sits on
    PanelState state;
    float repairProgress;    // 0..1, only meaningful while Repairing/Repaired
    int   wiresTotal;        // wires that must be connected to start Repairing
    int   wiresConnected;    // wires connected so far (the micro-task counter)
};

// Drives a set of repair panels off the player's position + the interact key. No
// render/physics ownership — pure state. The host queries panelCount()/panel(i) to
// draw + to read in-range/progress for HUD/visual feedback.
class RepairSystem {
public:
    // Register a damaged panel at world `pos`, facing `yaw`, requiring `wires`
    // connections (clamped to >=1). Starts Sealed, 0 wires connected.
    void addPanel(const float pos[3], float yaw, int wires);

    // Per-frame tick. `dt` advances any Repairing panel's progress timer. Finds the
    // panel whose center is within interactRange() of `playerPos` (nearest wins) and
    // returns its index, or -1 if none in range. If `interactPressed` is a RISING
    // edge (was up last frame, down now) AND the in-range panel is Sealed, it OPENS.
    // Returns the index of the in-range panel (regardless of state), or -1.
    int update(float dt, const float playerPos[3], bool interactPressed);

    // The micro-task: connect one more wire on panel `panelIndex` IF it is Open.
    // When the last wire connects, the panel transitions Open -> Repairing (progress
    // starts ramping in update()). No-op if out of range, not Open, or already full.
    void connectWire(int panelIndex);

    uint32_t panelCount() const { return (uint32_t)m_panels.size(); }
    const RepairPanel& panel(uint32_t i) const { return m_panels[i]; }

    // True iff there is >=1 panel AND every panel is Repaired.
    bool allRepaired() const;

    // Interaction reach (m). A panel within this distance of the player is "in range".
    float interactRange() const { return m_interactRange; }

    // Seconds a panel spends in Repairing (progress 0->1) once all wires connect.
    float repairDuration() const { return m_repairDuration; }

private:
    std::vector<RepairPanel> m_panels;
    bool  m_prevInteract   = false;   // for rising-edge detection on the interact key
    float m_interactRange  = 2.2f;    // matches the interior's console reach
    float m_repairDuration = 2.0f;    // short ramp — not a full minigame
};

// Headless self-test (--test-ship-repair, >=6 checks). Pure logic, no device:
//   T1 addPanel registers a Sealed panel with the requested wire count;
//   T2 update with the player FAR returns -1 (not in range), panel stays Sealed;
//   T3 update with the player NEAR returns the panel index (in range);
//   T4 a rising-edge interact in range OPENS a Sealed panel (Sealed -> Open);
//   T5 connectWire increments wiresConnected; the last wire flips Open -> Repairing;
//   T6 update ticks Repairing progress 0->1 over repairDuration, then -> Repaired;
//   T7 allRepaired() is false until the LAST panel is Repaired, then true.
// Logs PASS/FAIL T#; returns true iff all pass. Lives in ship_repair.cpp.
bool runShipRepairSelfTest();

} // namespace x3::space
