#pragma once
// ============================================================================
// THE WEAPON BENCH — where attachments are FITTED.
//
// Canon: Tim has LATE NIGHT SPEED for vehicle mods. A weapons bench is the same
// idea in the same world. You can SEE your mods in the backpack (I) anywhere; you
// can only FIT them here. Attachments are not a hot-swap menu you open mid-fight.
//
// WHERE: the SECURITY STATION on Floor 1 (canon room graph — a locked, secured
// room the player opens with the Security keycard/code, which already exists). The
// bench is a real object in that room: a steel table, a vice, a parts tray, and a
// HOLO PANEL over it.
//
// THE PANEL IS A HoloPanel (app/holo_panel.*, the ONE holo implementation, ece4f13).
// BLACK GLASS + shiny metallic round-pipe frame; ink is BLUE / GREEN / ORANGE and
// NEVER CYAN (--test-holoterm gates on that). It is a WallFlush variant: the back-box
// sits BEHIND the pane (landmine L3 — nothing may EVER go in front of a screen).
// Its content is baked by bakeBench(), a new BAKER on the existing platform. Adding
// a holo variant means adding a baker. It does not mean writing a second holo.
//
// [E] at the bench opens it. Arrow keys move, Left/Right cycle the candidate,
// Enter fits/removes, E/Esc closes. The panel re-bakes on every change (the
// readout IS the UI — it is diegetic, on the glass, and readable at [E] range).
// ============================================================================

#include "holo_panel.h"
#include "inventory.h"
#include "item_db.h"
#include "weapon.h"
#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace x3::game {

// The BENCH panel baker (a new variant on the HoloPanel platform).
//   `weapon`     — the weapon being worked on ("SHOTGUN")
//   `slotRows`   — one row per slot: { slot label, fitted-or-candidate text, state }
//   `statRows`   — the live before/after readout ("DAMAGE  20 -> 24")
//   `sel`        — highlighted row (0 = the weapon selector, 1..5 = the slots)
//   `hint`       — the bottom caption
// state: 0 = empty, 1 = fitted, 2 = not accepted by this weapon, 3 = a pending pick.
struct BenchRow {
    std::string label;
    std::string value;
    int         state = 0;
};
std::vector<uint8_t> bakeBench(uint32_t n, const std::string& weapon,
                               const std::vector<BenchRow>& slotRows,
                               const std::vector<BenchRow>& statRows,
                               int sel, const std::string& hint, float aspect = 1.0f);

class WeaponBench {
public:
    // Build the bench (table + vice + tray) and its HoloPanel at `pos` (the bench
    // FEET, world), facing `yaw`. `roomId` tags every entity for the per-room PVS cull.
    void build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld* physics,
               const x3::phys::Vec3& pos, float yaw, float ceilingY, uint32_t roomId);

    // Where the player must STAND to use it, and how close (the host's [E] chain
    // polls this — the same proximity pattern the keycard/terminal use).
    x3::phys::Vec3 usePos() const { return m_usePos; }
    float useRadius() const { return 2.2f; }
    bool inRange(const x3::phys::Vec3& p) const {
        const float dx = p.x - m_usePos.x, dz = p.z - m_usePos.z, dy = p.y - m_usePos.y;
        return m_built && (dx*dx + dz*dz) < useRadius()*useRadius() && std::fabs(dy) < 3.0f;
    }
    void shutdown(x3::rhi::IRenderDevice& device);
    bool built() const { return m_built; }

    x3::phys::Vec3 anchor() const { return m_panelPos; }
    // The panel's glow light (the host folds it into its own light rig).
    bool  hasGlowLight() const { return m_panel.hasGlowLight(); }
    const float* glowLightPos()   const { return m_panel.glowLightPos(); }
    const float* glowLightColor() const { return m_panel.glowLightColor(); }
    float glowLightRange() const { return m_panel.glowLightRange(); }

    // ---- The screen ---------------------------------------------------------
    bool open() const { return m_open; }
    // Rising-edge keys the host feeds while the bench screen is up.
    struct Input {
        bool up = false, down = false, left = false, right = false;
        bool activate = false;   // Enter: FIT / REMOVE
        bool close    = false;   // E / Esc
    };
    // Open the screen (the [E] handler calls this when the player is in range).
    void openScreen(Arsenal& arsenal, const Inventory& inv, const ItemDb& db);
    void closeScreen();
    // Drive the screen for one frame. Mutates the arsenal's loadouts + the backpack
    // (a fitted attachment LEAVES the bag; a removed one goes back into it). Calls
    // `onChanged` whenever a fit/unfit landed so the host can re-apply stats + save.
    void tick(const Input& in, Arsenal& arsenal, Inventory& inv, const ItemDb& db,
              const std::function<void()>& onChanged);
    // Advance the panel shimmer.
    void update(float dt) { m_panel.update(dt); }

    // True once the panel actually carries a baked screen (the regression guard).
    bool screenHasContent() const { return m_panel.screenHasContent(); }

private:
    void rebake(Arsenal& arsenal, const Inventory& inv, const ItemDb& db);
    // Candidate attachments for (weapon, slot): every attachment in the BACKPACK the
    // weapon's slot mask accepts, plus (if something is fitted) the fitted one at
    // index 0 so the player can see and remove it.
    std::vector<std::string> candidates(const Arsenal& a, int weapon, AttachSlot s,
                                        const Inventory& inv, const ItemDb& db) const;

    HoloPanel m_panel;
    bool      m_built = false;
    bool      m_open  = false;
    x3::phys::Vec3 m_panelPos{};
    x3::phys::Vec3 m_usePos{};
    float     m_aspect = 1.0f;
    uint32_t  m_texN   = 1024;   // the readout must be READ, not squinted at

    int m_weapon = 0;    // roster index being worked on
    int m_row    = 0;    // 0 = WEAPON selector, 1..5 = slots
    int m_cand[kAttachSlotCount] = { 0, 0, 0, 0, 0 };   // per-slot candidate cursor

    std::vector<uint32_t>            m_ents;
    std::vector<x3::rhi::MeshHandle> m_meshes;
};

} // namespace x3::game
