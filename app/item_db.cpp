// ITEM DATABASE (W9-3 RPG layer) — see item_db.h.
#include "item_db.h"
#include "asset_root.h"
#include "json_mini.h"
#include "story_ops.h"   // asciiFold (display text may carry authored UTF-8)

#include "engine/core/x3_log.h"

namespace x3::game {

const char* itemCategoryName(ItemCategory c) {
    switch (c) {
        case ItemCategory::Consumable:  return "consumable";
        case ItemCategory::Keycard:     return "keycard";
        case ItemCategory::Mod:         return "mod";
        case ItemCategory::UpgradePart: return "upgrade_part";
        case ItemCategory::Quest:       return "quest";
        default:                        return "unknown";
    }
}

ItemCategory itemCategoryFromName(std::string_view s) {
    if (s == "keycard")      return ItemCategory::Keycard;
    if (s == "mod")          return ItemCategory::Mod;
    if (s == "upgrade_part") return ItemCategory::UpgradePart;
    if (s == "quest")        return ItemCategory::Quest;
    return ItemCategory::Consumable;
}

std::string itemsJsonPath() { return assetRoot() + "/items/items.json"; }

const ItemDef* ItemDb::find(std::string_view id) const {
    for (const ItemDef& d : m_items) if (d.id == id) return &d;
    return nullptr;
}

bool ItemDb::load(std::string_view jsonPath) {
    m_items.clear();
    m_fromJson = false;
    const std::string text = jmini::readFile(std::string(jsonPath));
    if (!text.empty()) {
        jmini::JReader rd(text);
        jmini::JVal root = rd.parse();
        const jmini::JVal* items = root.get("items");
        if (rd.ok && items && items->t == jmini::JVal::Arr) {
            for (const jmini::JVal& j : items->arr) {
                if (j.t != jmini::JVal::Obj) continue;
                ItemDef d;
                d.id    = j.sval("id");
                if (d.id.empty()) continue;
                d.name  = asciiFold(j.sval("name", d.id));
                d.desc  = asciiFold(j.sval("desc"));
                d.cat   = itemCategoryFromName(j.sval("category", "consumable"));
                d.stack = j.inum("stack", 1); if (d.stack < 1) d.stack = 1;
                const std::string g = j.sval("glyph", "?");
                d.glyph = g.empty() ? '?' : g[0];
                if (const jmini::JVal* c = j.get("color"); c && c->t == jmini::JVal::Arr) {
                    for (size_t k = 0; k < 4 && k < c->arr.size(); ++k)
                        if (c->arr[k].t == jmini::JVal::Num) d.color[k] = (float)c->arr[k].num;
                }
                if (const jmini::JVal* e = j.get("effect"); e && e->t == jmini::JVal::Obj) {
                    d.fx.heal        = e->inum("heal", 0);
                    d.fx.ammo        = e->inum("ammo", 0);
                    d.fx.keycard     = e->inum("keycard", 0);
                    d.fx.xp          = e->inum("xp", 0);
                    d.fx.damageMult  = e->fnum("damageMult", 0.0f);
                    d.fx.reloadMult  = e->fnum("reloadMult", 0.0f);
                    d.fx.ammoCapMult = e->fnum("ammoCapMult", 0.0f);
                    d.fx.critChance  = e->fnum("critChance", 0.0f);
                }
                m_items.push_back(std::move(d));
            }
        }
        if (!m_items.empty()) {
            m_fromJson = true;
            x3::logInfo("[itemdb] loaded " + std::to_string(m_items.size()) +
                        " item defs from " + std::string(jsonPath));
            return true;
        }
    }
    bakeFallback();
    x3::logInfo("[itemdb] items JSON absent/invalid (" + std::string(jsonPath) +
                ") — baked fallback table (" + std::to_string(m_items.size()) + " defs)");
    return false;
}

// Baked minimal table: keeps the RPG layer + --test-inventory alive on a clean
// checkout. Mirrors assets/items/items.json (which is the authored superset).
void ItemDb::bakeFallback() {
    auto add = [&](const char* id, const char* name, ItemCategory cat, int stack,
                   char glyph, float r, float g, float b, ItemEffect fx, const char* desc) {
        ItemDef d; d.id = id; d.name = name; d.cat = cat; d.stack = stack;
        d.glyph = glyph; d.color[0] = r; d.color[1] = g; d.color[2] = b; d.color[3] = 1.0f;
        d.fx = fx; d.desc = desc;
        m_items.push_back(std::move(d));
    };
    ItemEffect fx;
    fx = {}; fx.heal = 35;
    add("medkit", "Medkit", ItemCategory::Consumable, 5, '+', 0.95f, 0.25f, 0.25f, fx,
        "Field medkit. Restores 35 HP.");
    fx = {}; fx.ammo = 30;
    add("ammo_pack", "Ammo Pack", ItemCategory::Consumable, 5, 'a', 0.95f, 0.75f, 0.20f, fx,
        "Loose rounds. +30 reserve ammo for the held weapon.");
    fx = {}; fx.heal = 100; fx.xp = 25;
    add("nano_booster", "Nano-Booster", ItemCategory::Consumable, 3, 'N', 0.20f, 0.95f, 0.75f, fx,
        "Salvari nanite serum. Fully restores HP.");
    fx = {}; fx.keycard = 1;
    add("keycard_security", "Security Keycard", ItemCategory::Keycard, 1, 'S', 0.15f, 0.88f, 1.00f, fx,
        "Security-grade access card. Opens Security-locked doors.");
    fx = {}; fx.keycard = 2;
    add("keycard_access", "Access Keycard", ItemCategory::Keycard, 1, 'K', 0.15f, 0.60f, 1.00f, fx,
        "Facility access card recovered from the upper floors.");
    fx = {}; fx.damageMult = 0.15f;
    add("mod_damage", "Overcharge Coil", ItemCategory::Mod, 1, 'D', 1.00f, 0.45f, 0.15f, fx,
        "Weapon mod. +15% damage on every weapon.");
    fx = {}; fx.reloadMult = 0.20f;
    add("mod_reload", "Assist Servo", ItemCategory::Mod, 1, 'R', 0.60f, 0.85f, 1.00f, fx,
        "Weapon mod. 20% faster reloads.");
    fx = {}; fx.ammoCapMult = 0.25f;
    add("mod_extmag", "Extended Racks", ItemCategory::Mod, 1, 'M', 0.75f, 0.75f, 0.30f, fx,
        "Weapon mod. +25% reserve ammo capacity.");
    fx = {};
    add("emp_parts", "EMP Components", ItemCategory::UpgradePart, 5, 'e', 0.55f, 0.55f, 0.95f, fx,
        "Scavenged capacitors and coils. The F4 Power Junction bench can build an EMP from these.");
    fx = {};
    add("antidote_parts", "Antidote Components", ItemCategory::UpgradePart, 5, 'v', 0.35f, 0.90f, 0.35f, fx,
        "Pharmacy compounds. Research notes mention an infection antidote.");
    fx = {};
    add("sarah_photo", "Photograph of Sarah", ItemCategory::Quest, 1, 'Q', 0.90f, 0.80f, 0.60f, fx,
        "A creased photograph. She is why you are still moving.");
}

} // namespace x3::game
