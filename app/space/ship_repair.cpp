// app/space/ship_repair.cpp — S7 in-transit panel-repair state machine.
#include "ship_repair.h"

#include <cmath>
#include <cstdio>

namespace x3::space {

void RepairSystem::addPanel(const float pos[3], float yaw, int wires) {
    if (wires < 1) wires = 1;
    RepairPanel p{};
    p.pos[0] = pos[0]; p.pos[1] = pos[1]; p.pos[2] = pos[2];
    p.yaw = yaw;
    p.state = PanelState::Sealed;
    p.repairProgress = 0.0f;
    p.wiresTotal = wires;
    p.wiresConnected = 0;
    m_panels.push_back(p);
}

int RepairSystem::update(float dt, const float playerPos[3], bool interactPressed) {
    // 1) Advance any Repairing panel toward Repaired (progress timer).
    if (dt > 0.0f && m_repairDuration > 0.0f) {
        for (auto& p : m_panels) {
            if (p.state == PanelState::Repairing) {
                p.repairProgress += dt / m_repairDuration;
                if (p.repairProgress >= 1.0f) {
                    p.repairProgress = 1.0f;
                    p.state = PanelState::Repaired;
                }
            }
        }
    }

    // 2) Find the NEAREST panel within interactRange of the player.
    int   inRange = -1;
    float bestD2  = m_interactRange * m_interactRange;
    for (uint32_t i = 0; i < m_panels.size(); ++i) {
        const RepairPanel& p = m_panels[i];
        const float dx = p.pos[0] - playerPos[0];
        const float dy = p.pos[1] - playerPos[1];
        const float dz = p.pos[2] - playerPos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; inRange = (int)i; }
    }

    // 3) Rising-edge interact opens an in-range Sealed panel.
    const bool rising = interactPressed && !m_prevInteract;
    if (rising && inRange >= 0) {
        RepairPanel& p = m_panels[(uint32_t)inRange];
        if (p.state == PanelState::Sealed) p.state = PanelState::Open;
    }
    m_prevInteract = interactPressed;

    return inRange;
}

void RepairSystem::connectWire(int panelIndex) {
    if (panelIndex < 0 || (uint32_t)panelIndex >= m_panels.size()) return;
    RepairPanel& p = m_panels[(uint32_t)panelIndex];
    if (p.state != PanelState::Open) return;
    if (p.wiresConnected >= p.wiresTotal) return;
    ++p.wiresConnected;
    if (p.wiresConnected >= p.wiresTotal) {
        p.state = PanelState::Repairing;   // progress starts ramping next update()
    }
}

bool RepairSystem::allRepaired() const {
    if (m_panels.empty()) return false;
    for (const auto& p : m_panels)
        if (p.state != PanelState::Repaired) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Headless self-test (--test-ship-repair)
// ---------------------------------------------------------------------------
bool runShipRepairSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; std::printf("  PASS %s\n", name); }
        else   {          std::printf("  FAIL %s\n", name); }
    };

    RepairSystem rs;
    const float panelPos[3] = { 2.0f, 1.2f, -2.0f };
    rs.addPanel(panelPos, 0.0f, 3);   // a Sealed panel needing 3 wires

    // T1: addPanel registers a Sealed panel with the requested wire count.
    check(rs.panelCount() == 1 &&
          rs.panel(0).state == PanelState::Sealed &&
          rs.panel(0).wiresTotal == 3 &&
          rs.panel(0).wiresConnected == 0,
          "T1 addPanel registers a Sealed panel (3 wires, 0 connected)");

    const float dt = 1.0f / 60.0f;

    // T2: player FAR -> not in range (-1), panel stays Sealed.
    {
        const float far[3] = { 20.0f, 1.2f, 20.0f };
        int idx = rs.update(dt, far, false);
        check(idx == -1 && rs.panel(0).state == PanelState::Sealed,
              "T2 player far -> -1 (not in range), stays Sealed");
    }

    // T3: player NEAR -> in range (returns the panel index).
    const float near[3] = { 2.4f, 1.2f, -2.0f };   // ~0.4 m from the panel
    {
        int idx = rs.update(dt, near, false);
        check(idx == 0, "T3 player near -> in range (index 0)");
    }

    // T4: rising-edge interact in range OPENS the Sealed panel.
    {
        // interactPressed false (above) -> now true == rising edge.
        int idx = rs.update(dt, near, true);
        check(idx == 0 && rs.panel(0).state == PanelState::Open,
              "T4 rising interact opens Sealed -> Open");
    }

    // T5: connectWire increments; the last wire flips Open -> Repairing.
    {
        rs.connectWire(0);
        bool one = rs.panel(0).wiresConnected == 1 && rs.panel(0).state == PanelState::Open;
        rs.connectWire(0);
        rs.connectWire(0);   // 3rd of 3 -> Repairing
        bool full = rs.panel(0).wiresConnected == 3 &&
                    rs.panel(0).state == PanelState::Repairing;
        check(one && full,
              "T5 connectWire increments; last wire -> Repairing");
    }

    // T6: update ticks Repairing progress 0->1 over repairDuration, then -> Repaired.
    {
        float prog0 = rs.panel(0).repairProgress;
        rs.update(dt, near, false);
        bool ramping = rs.panel(0).repairProgress > prog0;   // progress advanced
        // Run enough frames to exceed repairDuration.
        const int steps = (int)(rs.repairDuration() / dt) + 5;
        for (int i = 0; i < steps; ++i) rs.update(dt, near, false);
        bool done = rs.panel(0).state == PanelState::Repaired &&
                    rs.panel(0).repairProgress >= 1.0f;
        check(ramping && done,
              "T6 Repairing progress ramps 0->1 -> Repaired");
    }

    // T7: allRepaired false until the LAST panel done, then true.
    {
        check(rs.allRepaired(),
              "T7a allRepaired true after the only panel repaired");
        const float p2[3] = { -2.0f, 1.2f, 2.0f };
        rs.addPanel(p2, 0.0f, 1);   // a second, still-Sealed panel
        bool falseNow = !rs.allRepaired();
        // Drive the second panel through the full flow.
        const float at2[3] = { -2.2f, 1.2f, 2.0f };
        rs.update(dt, at2, false);
        rs.update(dt, at2, true);          // open it
        rs.connectWire(1);                 // 1 of 1 -> Repairing
        const int steps = (int)(rs.repairDuration() / dt) + 5;
        for (int i = 0; i < steps; ++i) rs.update(dt, at2, false);
        bool trueNow = rs.allRepaired();
        check(falseNow && trueNow,
              "T7b allRepaired flips false->true as the last panel completes");
    }

    std::printf("ship-repair: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
