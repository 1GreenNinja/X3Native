// Objective system + HUD objective text (Level 1 / §6.2). See app/objective.h.
//
// Clean-room: built from the IRenderDevice interface + std only.
#include "objective.h"

#include "engine/core/x3_log.h"

#include <string>

namespace x3::game {

namespace {
const std::string kEmpty;  // returned for "no current objective"
} // namespace

void ObjectiveSystem::set(const std::vector<std::string>& labels) {
    m_labels = labels;
    if (m_labels.empty()) {
        m_current = kNoObjective;
        m_started = false;
    } else {
        m_current = 0;
        m_started = true;
    }
}

void ObjectiveSystem::add(const std::string& label) {
    m_labels.push_back(label);
    if (m_current == kNoObjective && !m_started) {
        m_current = 0;
        m_started = true;
    }
}

uint32_t ObjectiveSystem::advance() {
    if (m_current == kNoObjective) return kNoObjective;  // already finished
    ++m_current;
    if (m_current >= (uint32_t)m_labels.size())
        m_current = kNoObjective;  // walked past the last objective
    return m_current;
}

const std::string& ObjectiveSystem::currentLabel() const {
    if (m_current == kNoObjective || m_current >= (uint32_t)m_labels.size())
        return kEmpty;
    return m_labels[m_current];
}

void ObjectiveSystem::drawCurrent(x3::rhi::IRenderDevice& device,
                                  const x3::rhi::FrameContext& frame) const {
    const std::string& label = currentLabel();
    if (label.empty()) return;
    // Top-left, just under the FPS meter line. White text, 16 px glyphs.
    const float color[4] = { 1.0f, 0.93f, 0.55f, 1.0f };  // warm objective-yellow
    const std::string line = "OBJECTIVE: " + label;
    device.drawHudText(frame, line.c_str(), 8.0f, 64.0f, 16.0f, color);
}

// ===========================================================================
// Headless self-test (folded into --test-level1). No device required.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[objective-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[objective-test] FAIL ") + name); }
}
} // namespace

bool runObjectiveSelfTest() {
    g_pass = g_fail = 0;

    ObjectiveSystem obj;
    check(obj.current() == kNoObjective && !obj.allComplete(),
          "fresh: no current objective, not complete");

    obj.set({ "Escape the detention cell",
              "Find weapons in the armory",
              "Reach the elevator to Floor 2",
              "Take the elevator" });
    check(obj.count() == 4 && obj.current() == 0 &&
          obj.currentLabel() == "Escape the detention cell",
          "set: cursor at first objective");

    obj.advance();
    check(obj.current() == 1 && obj.currentLabel() == "Find weapons in the armory",
          "advance 1 -> second objective");

    obj.complete();  // == advance
    check(obj.current() == 2 && obj.currentLabel() == "Reach the elevator to Floor 2",
          "complete 2 -> third objective");

    obj.advance();
    check(obj.current() == 3 && obj.currentLabel() == "Take the elevator",
          "advance 3 -> fourth objective");

    obj.advance();
    check(obj.current() == kNoObjective && obj.allComplete(),
          "advance past last -> finished + allComplete");

    // Advancing past the end is a stable no-op.
    obj.advance();
    check(obj.current() == kNoObjective && obj.allComplete(),
          "advance after finished is a no-op");

    x3::logInfo(std::string("[objective-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
