#pragma once
// Objective system + HUD objective text (Level 1 / §6.2).
//
// Game/slice code only — engine/ stays pure. A tiny ordered list of objective
// strings with a "current" cursor. The host advances the cursor as beats fire
// (see app/main.cpp), and draws the current objective line via the HUD text API
// (IRenderDevice::drawHudText, surfaced through Hud — see drawCurrent()).
//
// Maps to the bible's SetObjective / CompleteObjective. Deliberately minimal:
// the slice needs an ordered, advanceable list with a single visible line, not a
// full quest graph.

#include "engine/rhi/IRenderDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// Sentinel for "no current objective" (all complete, or none set).
constexpr uint32_t kNoObjective = 0xFFFFFFFFu;

// An ordered objective list with a current cursor. Each entry is a short label.
// The cursor starts at the first objective once set(); advance()/complete() move
// it forward; when it walks past the last entry the list is "done" (current() ==
// kNoObjective).
class ObjectiveSystem {
public:
    // Replace the objective list with `labels` and point the cursor at index 0.
    // An empty list leaves the cursor at kNoObjective.
    void set(const std::vector<std::string>& labels);

    // Append a single objective (cursor unchanged unless it was kNoObjective and
    // this is the first entry, in which case it points at index 0).
    void add(const std::string& label);

    // GTA-style free-text objective override (Lua x3.setObjective(text)). Sets a
    // single visible line that takes precedence over the list-driven currentLabel()
    // until cleared (empty text restores the list cursor). Lets pak script DATA set
    // the under-minimap objective without touching the ordered beat list.
    void setText(const std::string& text) { m_override = text; }
    const std::string& overrideText() const { return m_override; }

    // Advance the cursor to the next objective. No-op once past the end. Returns
    // the new current index (kNoObjective when the list is finished).
    uint32_t advance();

    // Mark the current objective complete (== advance for this minimal system).
    // Provided to read naturally at the call sites (beat sequence). Returns the
    // new current index.
    uint32_t complete() { return advance(); }

    // Index of the current objective, or kNoObjective if none/finished.
    uint32_t current() const { return m_current; }

    // Save/load restore: set the cursor directly to `index` (or kNoObjective to mark
    // the list finished). An in-range index points the cursor there and marks the
    // list started; kNoObjective marks it started+finished; an out-of-range index is
    // clamped to kNoObjective. Does not change the labels. Used by applyCheckpoint().
    void setCurrent(uint32_t index);

    // True once every objective has been completed (cursor past the end after a
    // non-empty list was set). False before any set() and while objectives remain.
    bool allComplete() const { return m_started && m_current == kNoObjective; }

    // Number of objectives in the list.
    uint32_t count() const { return (uint32_t)m_labels.size(); }

    // The current objective's label, or "" if none/finished.
    const std::string& currentLabel() const;

    // The label at index i (caller must pass i < count()).
    const std::string& labelAt(uint32_t i) const { return m_labels[i]; }

    // Draw the current objective line near the top-left of the screen via the HUD
    // text API. No-op if there is no current objective. Prefixes "OBJECTIVE: ".
    void drawCurrent(x3::rhi::IRenderDevice& device,
                     const x3::rhi::FrameContext& frame) const;

private:
    std::vector<std::string> m_labels;
    uint32_t                 m_current = kNoObjective;
    bool                     m_started = false;  // a non-empty list was set()
    std::string              m_override;         // free-text override (setText); wins when non-empty
};

// Headless self-test (folded into --test-level1, but standalone-callable). Drives
// set/advance/complete + the current()/allComplete() invariants with no device.
// Returns true iff all checks pass.
bool runObjectiveSelfTest();

} // namespace x3::game
