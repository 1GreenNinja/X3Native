#pragma once
// INVENTORY / BACKPACK (W9-3 RPG layer).
//
// A multi-slot, slot-capped backpack over the ItemDb:
//   * BACKPACK section — kBackpackSlots slots; consumables/mods/parts stack up
//     to their def's stack size; USE/DROP verbs operate here.
//   * KEY section — kKeySlots slots for keycards + quest items (non-consumable,
//     non-droppable; a locked door checks the bag via keycardMask()).
//
// The world pickups (CanonPlay's 32-item economy + the F1 Security keycard)
// deposit here through the host's item sink instead of instant-applying; if the
// backpack is FULL the sink refuses and the pickup stays in the world.
//
// Pure logic — no rendering, no physics. --test-inventory drives it headlessly
// (add/stack/consume/cap, keycard routing, a keycard gating a real Door).

#include "item_db.h"

#include <string>
#include <vector>

namespace x3::game {

// One inventory slot: an item id + count (empty when count == 0).
struct InvSlot {
    std::string itemId;
    int         count = 0;
    bool empty() const { return count <= 0; }
    void clear() { itemId.clear(); count = 0; }
};

class Inventory {
public:
    static constexpr int kBackpackSlots = 20;   // brief: start ~20
    static constexpr int kKeySlots      = 8;    // keycards + quest items

    // Add `n` of `def` (stacking, then first empty slot). Keycard/Quest items
    // route to the KEY section (one per slot, duplicates coalesce). Returns how
    // many were actually stored (0 = full — the caller leaves the pickup in the
    // world).
    int add(const ItemDef& def, int n = 1);

    // Remove up to `n` from a BACKPACK slot (consume/drop). Returns the number
    // removed. Key-section items never remove through this (use is gated in the
    // UI; keycards are permanent progression).
    int removeAt(int slot, int n = 1);

    // Total count of an item id across both sections.
    int countOf(std::string_view itemId) const;
    bool hasItem(std::string_view itemId) const { return countOf(itemId) > 0; }

    // OR of (1u << keycardId) for every keycard item held (KEY section). The
    // door-gating check in the host is `mask & (1u << door.keycard)` — this is
    // the bag-backed source of that mask.
    uint32_t keycardMask(const ItemDb& db) const;

    // ---- Accessors ---------------------------------------------------------
    const InvSlot& slot(int i) const { return m_slots[(size_t)i]; }
    const InvSlot& keySlot(int i) const { return m_keys[(size_t)i]; }
    int usedSlots() const;
    int usedKeySlots() const;

    // ---- Quick-use item (the HUD equipped-item chip) ------------------------
    // A BACKPACK slot index the player marked as "equipped" (Q uses it in-game).
    // -1 = none. The UI assigns it; the host clamps/clears when the slot empties.
    int  quickSlot() const { return m_quick; }
    void setQuickSlot(int s) { m_quick = (s >= -1 && s < kBackpackSlots) ? s : -1; }

    void clearAll();

    // ---- Persistence (text lines; rides the RPG save file) -----------------
    // serialize: "inv <slot> <id> <count>" / "invk <slot> <id> <count>" / "quick <slot>"
    std::string serialize() const;
    void deserialize(std::string_view text);   // replaces current state; unknown lines ignored

private:
    InvSlot m_slots[kBackpackSlots];
    InvSlot m_keys[kKeySlots];
    int     m_quick = -1;
};

// Headless self-test (--test-inventory): item DB load-or-bake; add/stack/
// consume/slot-cap; keycards route to the key section; keycardMask gates a real
// Door (locked+keycard refuses startOpening, mask bit + unlockAndOpen opens);
// serialize round-trip. Logs PASS/FAIL I#, returns true iff all pass.
bool runInventorySelfTest();

} // namespace x3::game
