#pragma once
// ITEM DATABASE (W9-3 RPG layer) — data-driven item definitions.
//
// Loads assets/items/items.json (x3.items/1) into a flat def table the
// Inventory + backpack UI + pickup wiring consume. On a missing/malformed JSON
// a baked-in fallback table keeps the whole RPG layer (and --test-inventory)
// working on a clean checkout — the same load-or-bake pattern GirlsDialog uses.
//
// Game/slice code only — engine/ stays pure. JSON via the shared jmini reader.

#include "attachments.h"   // AttachSpec — attachments ARE items (one data path)

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// Item categories (closed set; the JSON `category` string maps onto these).
// Consumable/Mod live in the BACKPACK section; Keycard/Quest live in the KEY
// section (never consumed, never dropped — they gate doors/story).
enum class ItemCategory : uint32_t {
    Consumable  = 0,   // medkit / ammo pack / nano-booster — USE applies + consumes
    Keycard     = 1,   // door-gating card (effect.keycard = the door keycard id)
    Mod         = 2,   // weapon mod — USE slots it onto the player's weapons
    UpgradePart = 3,   // crafting/upgrade material (EMP bench economy — Tier A #2)
    Quest       = 4,   // story item; held, not usable
    Attachment  = 5,   // WEAPON ATTACHMENT — carried in the bag, FITTED at the bench
                       // (never "used" from the backpack; ItemDef::attach is its spec)
    Count       = 6
};
const char* itemCategoryName(ItemCategory c);
ItemCategory itemCategoryFromName(std::string_view s);   // unknown -> Consumable

// The effect payload an item applies when USED (consumables/mods). All fields
// default to "no effect"; loaders only set what the JSON carries.
struct ItemEffect {
    int   heal        = 0;      // +HP on use (consumable)
    int   ammo        = 0;      // +reserve rounds to the current weapon (consumable)
    int   keycard     = 0;      // door keycard id this card carries (keycard)
    int   xp          = 0;      // XP granted on pickup/use (lore/booster flavor)
    // Mod stat layer (fractions; 0.15 = +15%). Folded into PlayerStatMods while
    // the mod is applied — layered on base, never mutating the WeaponDef table.
    float damageMult  = 0.0f;   // + weapon damage fraction
    float reloadMult  = 0.0f;   // - reload time fraction (0.2 = 20% faster)
    float ammoCapMult = 0.0f;   // + reserve ammo cap fraction
    float critChance  = 0.0f;   // + crit probability (crit = double damage)
};

// One item definition (the DB row).
struct ItemDef {
    std::string  id;            // stable string id ("medkit", "keycard_security")
    std::string  name;          // display name
    std::string  desc;          // description panel text (ASCII-folded at load)
    ItemCategory cat   = ItemCategory::Consumable;
    int          stack = 1;     // max count per backpack slot (>=1)
    char         glyph = '?';   // 1-char icon glyph drawn in the slot
    float        color[4] = { 0.8f, 0.8f, 0.8f, 1.0f };   // icon tint
    ItemEffect   fx;
    // WEAPON ATTACHMENT spec (category "attachment" + an "attach" JSON block).
    // .valid == false on every non-attachment item. This is why there is no second
    // attachment database: the item DB IS the attachment DB.
    AttachSpec   attach;
};

// The item DB: load-or-bake, id lookup, iteration.
class ItemDb {
public:
    // Load from `jsonPath` (assets/items/items.json). On any failure the baked
    // fallback table is installed instead (never leaves the DB empty). Returns
    // true iff the JSON parsed and carried at least one item.
    bool load(std::string_view jsonPath);

    bool fromJson() const { return m_fromJson; }
    uint32_t count() const { return (uint32_t)m_items.size(); }
    const ItemDef& at(uint32_t i) const { return m_items[i]; }
    const ItemDef* find(std::string_view id) const;

private:
    void bakeFallback();
    std::vector<ItemDef> m_items;
    bool m_fromJson = false;
};

// Resolve the items JSON path under the portable asset root
// (assetRoot() + "/items/items.json").
std::string itemsJsonPath();

} // namespace x3::game
