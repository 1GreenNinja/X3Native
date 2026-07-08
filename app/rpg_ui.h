#pragma once
// RPG SCREENS (W9-3): the BACKPACK screen (I), the SKILL TREE screen (K), and
// the in-game HUD chip (equipped quick-item + level/XP readout + level-up toast).
//
// Built entirely on the screen-space overlay path (UiContext over
// IRenderDevice::drawHudQuad/drawHudTextF) — the same layer the menus, world
// map, and production HUD use. The host opens/closes the screens on key edges
// (world-map pattern), freezes the sim + shows the cursor while one is open,
// and passes an input snapshot each frame. Mouse AND arrow-key navigable
// (controller-free per the brief).
//
// The screens MUTATE game state only through the host callbacks (use-item /
// stats-changed), so effect application (heal/ammo/mods -> Player/Arsenal)
// stays in app_run where those systems live.

#include "inventory.h"
#include "skilltree.h"
#include "progression.h"
#include "ui.h"

#include <functional>
#include <string>

namespace x3::game {

class RpgUi {
public:
    // Host-built per-frame input: the UiContext snapshot (mouse) + rising-edge
    // keys for grid navigation + verbs.
    struct Input {
        x3::ui::UiInput ui;      // mouse position/click (nav fields unused here)
        bool navUp    = false;   // arrow-key edges (move the grid selection)
        bool navDown  = false;
        bool navLeft  = false;
        bool navRight = false;
        bool activate = false;   // Enter: USE (backpack) / BUY (skills)
        bool dropKey  = false;   // X: drop one from the selected backpack slot
        bool equipKey = false;   // Q: set/clear the selected slot as the quick item
    };

    // Apply a consumable/mod's effect. Return true iff it was consumed (the UI
    // then decrements the slot). The host owns the actual effect application.
    using UseItemFn      = std::function<bool(const ItemDef&)>;
    // Fired after a skill purchase so the host re-applies the live stat layer.
    using StatsChangedFn = std::function<void()>;

    // ---- Open/close --------------------------------------------------------
    bool backpackOpen() const { return m_screen == Screen::Backpack; }
    bool skillsOpen()   const { return m_screen == Screen::Skills; }
    bool anyOpen()      const { return m_screen != Screen::None; }
    void toggleBackpack() { m_screen = backpackOpen() ? Screen::None : Screen::Backpack; }
    void toggleSkills()   { m_screen = skillsOpen()   ? Screen::None : Screen::Skills; }
    void closeAll()       { m_screen = Screen::None; }

    // ---- Screens (call while the matching screen is open) ------------------
    void drawBackpack(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      const Input& in, Inventory& inv, const ItemDb& db,
                      const UseItemFn& useItem);
    void drawSkills(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    const Input& in, SkillTree& tree, Progression& prog,
                    const StatsChangedFn& onChanged);

    // ---- In-game HUD chip (always, while playing) ---------------------------
    // Bottom-center: the equipped quick item (glyph/name/count + "[Q] USE") and
    // a compact "LV n" + XP progress sliver. Also runs the level-up toast.
    void drawHudChip(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const Inventory& inv, const ItemDb& db, const Progression& prog,
                     float dt);

    // Host calls when addXp reported a level gain -> center-screen toast.
    void notifyLevelUp(int newLevel) { m_toastLevel = newLevel; m_toastTimer = 4.0f; }

private:
    enum class Screen : uint8_t { None, Backpack, Skills };

    void drawScreenBackdrop(x3::ui::UiContext& ui, const char* title, const char* hint);

    Screen m_screen = Screen::None;

    // Backpack selection: 0..kBackpackSlots-1 = backpack grid, then
    // kBackpackSlots..+kKeySlots-1 = the key row.
    int m_invSel = 0;

    // Skills selection: index into the tree's node list (grid nav maps
    // branch=column, tier=row).
    int m_skillSel = 0;

    // Level-up toast state.
    int   m_toastLevel = 0;
    float m_toastTimer = 0.0f;

    float m_pulse = 0.0f;   // shared animation clock
};

} // namespace x3::game
