// INVENTORY / BACKPACK (W9-3 RPG layer) — see inventory.h.
#include "inventory.h"
#include "door.h"      // --test-inventory: a keycard gates a real Door

#include "engine/core/x3_log.h"

#include <cstdio>
#include <sstream>

namespace x3::game {

static bool isKeySection(ItemCategory c) {
    return c == ItemCategory::Keycard || c == ItemCategory::Quest;
}

int Inventory::add(const ItemDef& def, int n) {
    if (n <= 0) return 0;
    int stored = 0;
    if (isKeySection(def.cat)) {
        // KEY section: coalesce onto an existing slot, else first empty.
        for (InvSlot& s : m_keys) {
            if (!s.empty() && s.itemId == def.id) { s.count += n; return n; }
        }
        for (InvSlot& s : m_keys) {
            if (s.empty()) { s.itemId = def.id; s.count = n; return n; }
        }
        return 0;   // key ring full (8 slots; unlikely in practice)
    }
    // BACKPACK: top up existing stacks first.
    for (InvSlot& s : m_slots) {
        if (stored >= n) break;
        if (!s.empty() && s.itemId == def.id && s.count < def.stack) {
            const int room = def.stack - s.count;
            const int take = (n - stored) < room ? (n - stored) : room;
            s.count += take; stored += take;
        }
    }
    // Then open new slots.
    for (InvSlot& s : m_slots) {
        if (stored >= n) break;
        if (s.empty()) {
            const int take = (n - stored) < def.stack ? (n - stored) : def.stack;
            s.itemId = def.id; s.count = take; stored += take;
        }
    }
    return stored;
}

int Inventory::removeAt(int slot, int n) {
    if (slot < 0 || slot >= kBackpackSlots || n <= 0) return 0;
    InvSlot& s = m_slots[(size_t)slot];
    if (s.empty()) return 0;
    const int take = n < s.count ? n : s.count;
    s.count -= take;
    if (s.count <= 0) s.clear();
    if (m_quick == slot && s.empty()) m_quick = -1;   // equipped stack emptied
    return take;
}

int Inventory::countOf(std::string_view itemId) const {
    int c = 0;
    for (const InvSlot& s : m_slots) if (!s.empty() && s.itemId == itemId) c += s.count;
    for (const InvSlot& s : m_keys)  if (!s.empty() && s.itemId == itemId) c += s.count;
    return c;
}

uint32_t Inventory::keycardMask(const ItemDb& db) const {
    uint32_t mask = 0;
    for (const InvSlot& s : m_keys) {
        if (s.empty()) continue;
        const ItemDef* d = db.find(s.itemId);
        if (d && d->cat == ItemCategory::Keycard && d->fx.keycard > 0 && d->fx.keycard < 32)
            mask |= (1u << (uint32_t)d->fx.keycard);
    }
    return mask;
}

int Inventory::usedSlots() const {
    int c = 0; for (const InvSlot& s : m_slots) if (!s.empty()) ++c; return c;
}
int Inventory::usedKeySlots() const {
    int c = 0; for (const InvSlot& s : m_keys) if (!s.empty()) ++c; return c;
}

void Inventory::clearAll() {
    for (InvSlot& s : m_slots) s.clear();
    for (InvSlot& s : m_keys)  s.clear();
    m_quick = -1;
}

std::string Inventory::serialize() const {
    std::ostringstream ss;
    for (int i = 0; i < kBackpackSlots; ++i)
        if (!m_slots[i].empty())
            ss << "inv " << i << ' ' << m_slots[i].itemId << ' ' << m_slots[i].count << '\n';
    for (int i = 0; i < kKeySlots; ++i)
        if (!m_keys[i].empty())
            ss << "invk " << i << ' ' << m_keys[i].itemId << ' ' << m_keys[i].count << '\n';
    if (m_quick >= 0) ss << "quick " << m_quick << '\n';
    return ss.str();
}

void Inventory::deserialize(std::string_view text) {
    clearAll();
    std::istringstream ss{ std::string(text) };
    std::string line;
    while (std::getline(ss, line)) {
        std::istringstream ls(line);
        std::string tag; ls >> tag;
        if (tag == "inv" || tag == "invk") {
            int slot = -1, count = 0; std::string id;
            ls >> slot >> id >> count;
            if (id.empty() || count <= 0) continue;
            if (tag == "inv" && slot >= 0 && slot < kBackpackSlots) {
                m_slots[(size_t)slot].itemId = id; m_slots[(size_t)slot].count = count;
            } else if (tag == "invk" && slot >= 0 && slot < kKeySlots) {
                m_keys[(size_t)slot].itemId = id; m_keys[(size_t)slot].count = count;
            }
        } else if (tag == "quick") {
            int q = -1; ls >> q; setQuickSlot(q);
        }
    }
}

// ===========================================================================
// --test-inventory
// ===========================================================================
static int g_ipass = 0, g_ifail = 0;
static void icheck(bool ok, const char* what) {
    if (ok) { ++g_ipass; x3::logInfo(std::string("  PASS I") + std::to_string(g_ipass + g_ifail) + " " + what); }
    else    { ++g_ifail; x3::logError(std::string("  FAIL I") + std::to_string(g_ipass + g_ifail) + " " + what); }
}

bool runInventorySelfTest() {
    g_ipass = g_ifail = 0;

    // I1: the DB always resolves (JSON if present, else the baked table) and
    // carries the core economy ids.
    ItemDb db;
    const bool fromJson = db.load(itemsJsonPath());
    icheck(db.count() >= 10 && db.find("medkit") && db.find("ammo_pack") &&
           db.find("keycard_security") && db.find("mod_damage"),
           "item DB resolves (json-or-baked) with the core economy ids");
    x3::logInfo(std::string("    (item defs: ") + std::to_string(db.count()) +
                (fromJson ? ", from JSON)" : ", baked fallback)"));

    const ItemDef& medkit = *db.find("medkit");
    const ItemDef& card   = *db.find("keycard_security");

    // I2: add + stacking. Medkit stack=5: 3 then 4 -> slot0=5, slot1=2.
    Inventory inv;
    icheck(inv.add(medkit, 3) == 3 && inv.add(medkit, 4) == 4 &&
           inv.slot(0).count == 5 && inv.slot(1).count == 2 && inv.countOf("medkit") == 7,
           "add + stack: 3+4 medkits -> full stack of 5 + overflow stack of 2");

    // I3: consume. Remove 2 from slot 0 -> 3 left; empty a slot -> cleared.
    icheck(inv.removeAt(0, 2) == 2 && inv.slot(0).count == 3 &&
           inv.removeAt(1, 5) == 2 && inv.slot(1).empty(),
           "consume: removeAt decrements and clears emptied slots");

    // I4: slot cap. Fill all 20 slots with full medkit stacks; the next add refuses.
    Inventory full;
    for (int i = 0; i < Inventory::kBackpackSlots; ++i) full.add(medkit, medkit.stack);
    icheck(full.usedSlots() == Inventory::kBackpackSlots && full.add(medkit, 1) == 0,
           "slot cap: a full 20-slot backpack refuses the 21st stack");

    // I5: keycards route to the KEY section (backpack slots untouched) and are
    // not removable through the backpack verb.
    Inventory keys;
    icheck(keys.add(card, 1) == 1 && keys.usedSlots() == 0 && keys.usedKeySlots() == 1 &&
           keys.removeAt(0, 1) == 0 && keys.hasItem("keycard_security"),
           "keycards live in the key section and cannot be dropped/consumed");

    // I6: a keycard in the bag gates a real Door. The host formula is
    // `keycardMask & (1u << door.keycard)`; without the card the locked door
    // refuses startOpening, with it the host unlockAndOpen()s it.
    {
        DoorSystem ds;
        Door d;
        d.locked  = true;
        d.keycard = kKeycardSecurity;
        const bool refusedLocked = !ds.startOpening(d) && d.state == DoorState::Closed;
        Inventory bag;
        const bool noCardNoBit = (bag.keycardMask(db) & (1u << (uint32_t)d.keycard)) == 0;
        bag.add(card, 1);
        const bool cardSetsBit = (bag.keycardMask(db) & (1u << (uint32_t)d.keycard)) != 0;
        const bool opened = cardSetsBit && ds.unlockAndOpen(d) && d.state == DoorState::Opening;
        icheck(refusedLocked && noCardNoBit && cardSetsBit && opened,
               "keycard gates a door: locked refuses, bag mask bit opens it");
    }

    // I7: serialize round-trip (backpack + keys + quick slot).
    {
        Inventory a;
        a.add(medkit, 4);
        a.add(*db.find("ammo_pack"), 2);
        a.add(card, 1);
        a.setQuickSlot(0);
        Inventory b;
        b.deserialize(a.serialize());
        icheck(b.countOf("medkit") == 4 && b.countOf("ammo_pack") == 2 &&
               b.countOf("keycard_security") == 1 && b.quickSlot() == 0 &&
               b.usedKeySlots() == 1,
               "serialize/deserialize round-trip preserves both sections + quick slot");
    }

    x3::logInfo("--test-inventory: " + std::to_string(g_ipass) + " passed, " +
                std::to_string(g_ifail) + " failed");
    return g_ifail == 0;
}

} // namespace x3::game
