// ITEM DATABASE (W9-3 RPG layer) — see item_db.h.
#include "item_db.h"
#include "asset_root.h"
#include "json_mini.h"
#include "story_ops.h"   // asciiFold (display text may carry authored UTF-8)

#include "engine/core/x3_log.h"
#include "engine/core/x3_damage.h"   // baked attachment coatings name DamageType

namespace x3::game {

const char* itemCategoryName(ItemCategory c) {
    switch (c) {
        case ItemCategory::Consumable:  return "consumable";
        case ItemCategory::Keycard:     return "keycard";
        case ItemCategory::Mod:         return "mod";
        case ItemCategory::UpgradePart: return "upgrade_part";
        case ItemCategory::Quest:       return "quest";
        case ItemCategory::Attachment:  return "attachment";
        default:                        return "unknown";
    }
}

ItemCategory itemCategoryFromName(std::string_view s) {
    if (s == "keycard")      return ItemCategory::Keycard;
    if (s == "mod")          return ItemCategory::Mod;
    if (s == "upgrade_part") return ItemCategory::UpgradePart;
    if (s == "quest")        return ItemCategory::Quest;
    if (s == "attachment")   return ItemCategory::Attachment;
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
                // ---- WEAPON ATTACHMENT block ("attach") ------------------------
                // Same load-or-bake pattern; a malformed/absent block simply leaves
                // attach.valid == false and the item is not an attachment.
                if (const jmini::JVal* a = j.get("attach");
                        a && a->t == jmini::JVal::Obj && d.cat == ItemCategory::Attachment) {
                    AttachSpec& s = d.attach;
                    s.id   = d.id;
                    s.name = d.name;
                    const std::string sn = a->sval("slot", "");
                    s.slot = attachSlotFromName(sn);
                    s.valid = !sn.empty();
                    if (const jmini::JVal* st = a->get("stats"); st && st->t == jmini::JVal::Obj) {
                        s.stats.damageMult   = st->fnum("damageMult",   0.0f);
                        s.stats.spreadMult   = st->fnum("spreadMult",   0.0f);
                        s.stats.recoilMult   = st->fnum("recoilMult",   0.0f);
                        s.stats.rangeMult    = st->fnum("rangeMult",    0.0f);
                        s.stats.magMult      = st->fnum("magMult",      0.0f);
                        s.stats.reloadMult   = st->fnum("reloadMult",   0.0f);
                        s.stats.ammoCapMult  = st->fnum("ammoCapMult",  0.0f);
                        s.stats.noiseMult    = st->fnum("noiseMult",    0.0f);
                        s.stats.flashScale   = st->fnum("flashScale",   0.0f);
                        s.stats.handlingMult = st->fnum("handlingMult", 0.0f);
                        s.stats.critChance   = st->fnum("critChance",   0.0f);
                        s.stats.damageType   = st->inum("damageType",  -1);
                    }
                    if (const jmini::JVal* o = a->get("optic"); o && o->t == jmini::JVal::Obj) {
                        s.optic.isOptic   = true;
                        s.optic.sightRise  = o->fnum("sightRise",  s.optic.sightRise);
                        s.optic.sightAlong = o->fnum("sightAlong", s.optic.sightAlong);
                        s.optic.adsFovDeg  = o->fnum("adsFovDeg",  s.optic.adsFovDeg);
                        s.optic.adsTime    = o->fnum("adsTime",    s.optic.adsTime);
                        s.optic.sway       = o->fnum("sway",       s.optic.sway);
                        s.optic.fullScope  = o->inum("fullScope", 0) != 0;
                    }
                    s.part      = attachPartFromName(a->sval("part", "none"));
                    s.metallic  = a->fnum("metallic",  s.metallic);
                    s.roughness = a->fnum("roughness", s.roughness);
                    if (const jmini::JVal* c = a->get("albedo"); c && c->t == jmini::JVal::Arr)
                        for (size_t k = 0; k < 3 && k < c->arr.size(); ++k)
                            if (c->arr[k].t == jmini::JVal::Num) s.albedo[k] = (float)c->arr[k].num;
                    if (const jmini::JVal* c = a->get("emissive"); c && c->t == jmini::JVal::Arr)
                        for (size_t k = 0; k < 3 && k < c->arr.size(); ++k)
                            if (c->arr[k].t == jmini::JVal::Num) s.emissive[k] = (float)c->arr[k].num;
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

    // ---- WEAPON ATTACHMENTS (mirrors the "attach" blocks in items.json) ------
    // Same load-or-bake contract as the rest of the table: a clean checkout with no
    // assets still boots the bench, the fits, and --test-attachments.
    auto addAtt = [&](const char* id, const char* name, char glyph, AttachSlot slot,
                      AttachPart part, AttachStats st, const char* desc,
                      float ar, float ag, float ab, float metal, float rough,
                      const OpticSpec* optic = nullptr,
                      float er = 0.0f, float eg = 0.0f, float eb = 0.0f) {
        ItemDef d;
        d.id = id; d.name = name; d.cat = ItemCategory::Attachment; d.stack = 1;
        d.glyph = glyph; d.desc = desc;
        d.color[0] = ar > 0.5f ? ar : ar + 0.4f; d.color[1] = ag + 0.35f; d.color[2] = ab + 0.35f;
        d.color[3] = 1.0f;
        AttachSpec& s = d.attach;
        s.valid = true; s.id = id; s.name = name; s.slot = slot; s.stats = st; s.part = part;
        s.albedo[0] = ar; s.albedo[1] = ag; s.albedo[2] = ab;
        s.metallic = metal; s.roughness = rough;
        s.emissive[0] = er; s.emissive[1] = eg; s.emissive[2] = eb;
        if (optic) s.optic = *optic;
        m_items.push_back(std::move(d));
    };
    AttachStats st;
    st = {}; st.damageMult = -0.15f; st.noiseMult = -0.70f; st.flashScale = -0.65f;
    addAtt("att_suppressor", "Suppressor", 'S', AttachSlot::Barrel, AttachPart::Suppressor, st,
           "BARREL. Heard at a third the distance, almost no flash. -15% damage.",
           0.33f, 0.335f, 0.34f, 0.35f, 0.50f);
    st = {}; st.spreadMult = -0.35f; st.recoilMult = -0.40f; st.flashScale = 0.90f; st.noiseMult = 0.25f;
    addAtt("att_compensator", "Compensator", 'C', AttachSlot::Barrel, AttachPart::Compensator, st,
           "BARREL. The gun stops climbing - and vents a much bigger, louder flash.",
           0.40f, 0.385f, 0.35f, 0.35f, 0.40f);
    st = {}; st.damageMult = 0.20f; st.rangeMult = 0.35f; st.handlingMult = -0.25f; st.reloadMult = 0.10f;
    addAtt("att_heavy_barrel", "Heavy Barrel", 'H', AttachSlot::Barrel, AttachPart::HeavyBarrel, st,
           "BARREL. Hits harder, reaches further, drags the whole gun around behind it.",
           0.37f, 0.38f, 0.39f, 0.35f, 0.38f);
    st = {}; st.magMult = 0.40f; st.ammoCapMult = 0.25f; st.reloadMult = 0.30f;
    addAtt("att_ext_mag", "Extended Magazine", 'E', AttachSlot::Magazine, AttachPart::ExtMag, st,
           "MAGAZINE. +40% rounds, deeper reserve. Reloads take 30% longer.",
           0.34f, 0.34f, 0.355f, 0.20f, 0.65f);
    st = {}; st.reloadMult = -0.35f; st.magMult = -0.25f;
    addAtt("att_fast_mag", "Fast Magazine", 'F', AttachSlot::Magazine, AttachPart::FastMag, st,
           "MAGAZINE. 35% faster reloads, a quarter fewer rounds.",
           0.36f, 0.375f, 0.39f, 0.25f, 0.55f);
    OpticSpec op;
    st = {}; st.spreadMult = -0.30f; st.critChance = 0.05f; st.handlingMult = -0.05f;
    op = {}; op.isOptic = true; op.sightRise = 0.105f; op.sightAlong = 0.06f;
    op.adsFovDeg = 46.0f; op.adsTime = 0.12f; op.sway = 0.0f; op.fullScope = false;
    addAtt("att_reflex", "Reflex Sight", 'R', AttachSlot::Optic, AttachPart::Reflex, st,
           "OPTIC. Open red dot. Snaps up fast, barely narrows your view.",
           0.31f, 0.315f, 0.32f, 0.30f, 0.45f, &op);
    st = {}; st.spreadMult = -0.45f; st.critChance = 0.08f; st.rangeMult = 0.25f; st.handlingMult = -0.18f;
    op = {}; op.isOptic = true; op.sightRise = 0.125f; op.sightAlong = 0.08f;
    op.adsFovDeg = 28.0f; op.adsTime = 0.26f; op.sway = 0.12f; op.fullScope = false;
    addAtt("att_acog", "Combat Optic", 'A', AttachSlot::Optic, AttachPart::Scope, st,
           "OPTIC. 2x prism. Real reach; the world outside the glass goes away.",
           0.32f, 0.33f, 0.32f, 0.30f, 0.38f, &op);
    st = {}; st.spreadMult = -0.60f; st.critChance = 0.12f; st.rangeMult = 0.50f; st.handlingMult = -0.35f;
    op = {}; op.isOptic = true; op.sightRise = 0.135f; op.sightAlong = 0.10f;
    op.adsFovDeg = 14.0f; op.adsTime = 0.42f; op.sway = 0.30f; op.fullScope = true;
    addAtt("att_scope", "Sniper Scope", 'X', AttachSlot::Optic, AttachPart::Scope, st,
           "OPTIC. 4x glass, full scope picture, drifting reticle, barge-like turn.",
           0.30f, 0.305f, 0.315f, 0.30f, 0.33f, &op);
    st = {}; st.spreadMult = -0.50f; st.magMult = -0.15f;
    addAtt("att_laser", "Targeting Laser", 'L', AttachSlot::Underbarrel, AttachPart::Laser, st,
           "UNDERBARREL. Halves hipfire spread. Feeds off the mag, and paints a line back to you.",
           0.35f, 0.35f, 0.36f, 0.30f, 0.40f, nullptr, 1.6f, 0.10f, 0.10f);
    st = {}; st.recoilMult = -0.35f; st.handlingMult = 0.08f; st.rangeMult = -0.15f;
    addAtt("att_grip", "Fore Grip", 'G', AttachSlot::Underbarrel, AttachPart::Grip, st,
           "UNDERBARREL. -35% recoil, quicker swing, gives up reach.",
           0.24f, 0.24f, 0.25f, 0.05f, 0.85f);
    st = {}; st.damageType = (int)x3::DamageType::Energy; st.critChance = 0.06f;
    st.damageMult = -0.10f; st.reloadMult = 0.15f;
    addAtt("att_salvari_coating", "Salvari Coating", 'V', AttachSlot::Coating, AttachPart::Cell, st,
           "COATING. Every shot leaves as ENERGY. The lattice bleeds charge.",
           0.27f, 0.34f, 0.30f, 0.30f, 0.30f, nullptr, 0.10f, 1.30f, 0.55f);
    st = {}; st.damageType = (int)x3::DamageType::Kinetic; st.damageMult = 0.12f; st.noiseMult = 0.35f;
    addAtt("att_kinetic_coating", "Kinetic Sheath", 'K', AttachSlot::Coating, AttachPart::Cell, st,
           "COATING. Every shot leaves as KINETIC and hits harder. It cracks like a cannon.",
           0.38f, 0.32f, 0.24f, 0.30f, 0.35f, nullptr, 1.30f, 0.55f, 0.10f);
}

} // namespace x3::game
