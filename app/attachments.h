#pragma once
// ============================================================================
// WEAPON ATTACHMENTS — the data layer (slots, stats, loadouts, the ONE fold).
//
// Attachments are ITEMS (assets/items/items.json, category "attachment"), so the
// item DB is the single data path — there is NO second attachment database. An
// attachment declares a SLOT, a block of stat FRACTIONS, an optional visible PART,
// and (for optics) a real sight/ADS spec.
//
// FIVE SLOTS. A weapon declares which it accepts via WeaponDef::attachSlots (a
// bitmask) — a pistol takes no underbarrel, the BFG is Coating-only.
//
//   Optic       — sights. NOT a stat: a REAL scope (true ADS through the glass).
//   Barrel      — suppressor / compensator / heavy barrel. Mounts at the MEASURED
//                 WeaponDef::vmMuzzle (346f5e7) — no new hardcoded offsets.
//   Magazine    — extended / fast.
//   Underbarrel — laser / grip.
//   Coating     — the ENERGY slot (Salvari tech). Sets the shot's DamageType and
//                 carries the energy stat block. It is deliberately a PLAIN
//                 multiplier block + a type override so a CHARGE MODEL landing
//                 separately (feat/weapons-overhaul) can compose with it: read the
//                 fitted coating id / DamageType off the effective WeaponDef and
//                 add drain/recharge on top. Nothing here owns charge.
//
// ---------------------------------------------------------------------------
// THE STACKING RULE (decided; documented; asserted in --test-attachments)
// ---------------------------------------------------------------------------
// There are TWO independent layers and ONE place each is folded:
//
//   LAYER 1  ATTACHMENTS -> applyAttachments(base, loadout) -> an EFFECTIVE
//            WeaponDef. Every stat field here is a FRACTION (0.20 = +20%) and
//            slots compose MULTIPLICATIVELY: mult = prod over slots of (1 + f).
//            critChance is a FLAT probability and composes ADDITIVELY.
//            This is the ONLY place a WeaponDef is modified. The roster is never
//            mutated: Arsenal keeps base defs and a parallel effective cache.
//
//   LAYER 2  SKILLS + MOD ITEMS -> PlayerStatMods (skilltree.h), applied at the
//            existing read points (rpgScaleDamage / setReloadMult / setAmmoCapMult
//            / setSpeedMult). UNCHANGED by this feature.
//
//   The two layers MULTIPLY (mults) / ADD (flat bonuses, crit). They are
//   independent: a +15% damage skill on a +20% heavy barrel gives 1.15 * 1.20.
//   Damage is quantized ONCE, at the end of layer 1 (an int on the WeaponDef);
//   layer 2 scales that int at the fire site. That ordering is asserted.
//
// Pure logic — no GPU, no Scene, no includes beyond <string>. The visible parts
// live in attach_view.*, the bench in weapon_bench.*, and --test-attachments
// drives all of this headlessly.
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// The five slots.
// ---------------------------------------------------------------------------
enum class AttachSlot : uint8_t {
    Optic       = 0,
    Barrel      = 1,
    Magazine    = 2,
    Underbarrel = 3,
    Coating     = 4,
    Count       = 5,
};
inline constexpr int kAttachSlotCount = (int)AttachSlot::Count;

// Slot mask bits (WeaponDef::attachSlots).
inline constexpr uint8_t slotBit(AttachSlot s) { return (uint8_t)(1u << (uint8_t)s); }
inline constexpr uint8_t kSlotsAll = 0x1Fu;   // all five

const char* attachSlotName(AttachSlot s);
AttachSlot  attachSlotFromName(std::string_view s);   // unknown -> Coating (and !valid on the spec)

// ---------------------------------------------------------------------------
// The stat block. EVERY field is a FRACTION of base (0.20 = +20%, -0.15 = -15%),
// EXCEPT critChance (a flat probability) and damageType (an override).
// ---------------------------------------------------------------------------
struct AttachStats {
    float damageMult   = 0.0f;   // + weapon damage
    float spreadMult   = 0.0f;   // + cone half-angle  (NEGATIVE = tighter)
    float recoilMult   = 0.0f;   // + camera kick      (NEGATIVE = softer)
    float rangeMult    = 0.0f;   // + effective range
    float magMult      = 0.0f;   // + magazine size
    float reloadMult   = 0.0f;   // + reload TIME      (NEGATIVE = faster)
    float ammoCapMult  = 0.0f;   // + reserve ammo cap
    float noiseMult    = 0.0f;   // + gunshot HEARING radius (-0.70 = heard at 30%)
    float flashScale   = 0.0f;   // + muzzle-flash size (visibility)
    float handlingMult = 0.0f;   // + turn speed (NEGATIVE = slower / heavier)
    float critChance   = 0.0f;   // FLAT crit probability added (additive layer)
    int   damageType   = -1;     // -1 = keep the weapon's type; else x3::DamageType
};

// ---------------------------------------------------------------------------
// OPTIC spec — the Optic slot is a FEATURE, not a number. This is the real
// sight: where the glass sits on the gun, what the camera does when you bring it
// up, and how it costs you.
// ---------------------------------------------------------------------------
struct OpticSpec {
    bool  isOptic   = false;
    // Sight height ABOVE the barrel line, in the weapon's own barrel-run units
    // (see attachSightLocal()) — this is where the LENS CENTER sits, and it is the
    // single point BOTH the visible optic mesh and the ADS alignment solve use.
    // If the drawn glass and the aim solve ever disagree, the reticle lies.
    float sightRise  = 0.115f;
    float sightAlong = 0.06f;    // fraction of the MUZZLE DISTANCE from the model origin
                                 // (0 = on the receiver, 1 = at the barrel tip)
    float adsFovDeg  = 42.0f;    // camera FOV while scoped (base is 60) — REAL magnification
    float adsTime    = 0.18f;    // seconds to raise/lower the sight
    float sway       = 0.0f;     // scope drift half-amplitude (degrees); 0 = rock steady
    bool  fullScope  = false;    // true: black scope picture (sniper); false: open red dot
};
// Base camera FOV the ADS FOV is measured against (matches the live loop's setCamera).
inline constexpr float kBaseFovDeg = 60.0f;

// ---------------------------------------------------------------------------
// The visible PART (attach_view.cpp maps `part` onto a procedural mesh + mount).
// ---------------------------------------------------------------------------
enum class AttachPart : uint8_t {
    None = 0, Suppressor, Compensator, HeavyBarrel,
    ExtMag, FastMag, Reflex, Scope, Laser, Grip, Cell,
};
AttachPart attachPartFromName(std::string_view s);

// ---------------------------------------------------------------------------
// One attachment's full definition — carried ON the ItemDef (ItemDef::attach), so
// items.json stays the single data path.
// ---------------------------------------------------------------------------
struct AttachSpec {
    bool        valid = false;                 // false = this item is not an attachment
    std::string id;                            // the ItemDef id (echoed for the loadout)
    std::string name;
    AttachSlot  slot = AttachSlot::Coating;
    AttachStats stats;
    OpticSpec   optic;
    AttachPart  part = AttachPart::None;       // visible mesh (None = invisible fit)
    // HONEST PBR for the part (no fake emissive, no near-white albedo, no over-unity).
    float albedo[3]  = { 0.16f, 0.17f, 0.18f };
    float metallic   = 0.85f;
    float roughness  = 0.42f;
    float emissive[3]= { 0.0f, 0.0f, 0.0f };   // only a small LIT CORE may glow (energy cell / laser)
};

// ---------------------------------------------------------------------------
// A weapon's fitted loadout: one AttachSpec per slot (invalid = empty).
// ---------------------------------------------------------------------------
struct WeaponLoadout {
    AttachSpec slots[kAttachSlotCount];
    bool has(AttachSlot s) const { return slots[(int)s].valid; }
    const AttachSpec* get(AttachSlot s) const {
        const AttachSpec& a = slots[(int)s];
        return a.valid ? &a : nullptr;
    }
    const AttachSpec* optic() const { return get(AttachSlot::Optic); }
    int  fittedCount() const;
    void clear();
};

// Every weapon's loadout, indexed by roster slot. Owned by the Arsenal.
class Loadouts {
public:
    void resize(int weapons);
    int  size() const { return (int)m_w.size(); }
    const WeaponLoadout& at(int weapon) const;
    WeaponLoadout&       mut(int weapon);

    // THE SLOT-MASK RULE. An attachment fits weapon `w` iff the spec is valid AND
    // the weapon's attachSlots mask carries that slot's bit. You cannot put an
    // optic in a barrel slot: the slot is a property of the ATTACHMENT, and the
    // weapon either accepts that slot or it does not.
    static bool canFit(uint8_t weaponSlotMask, const AttachSpec& a);

    // Fit `a` on weapon `w` (whose accept mask is `weaponSlotMask`). Returns false
    // and changes nothing if canFit fails / `w` is out of range. Any attachment
    // already in that slot is returned through `displaced` (empty = none) so the
    // host can put it back in the backpack.
    bool fit(int weapon, uint8_t weaponSlotMask, const AttachSpec& a, std::string* displaced = nullptr);

    // Remove whatever is in `s` on weapon `w`. Returns its item id ("" = nothing).
    std::string unfit(int weapon, AttachSlot s);

    // ---- Persistence (text lines, rides the RPG save file like "skill "/"mod ") --
    //   "attach <weaponIndex> <slotName> <itemId>"
    std::string serialize() const;
    // Re-resolve every line through `resolve` (id -> AttachSpec*) + `maskOf`
    // (weapon index -> accept mask), so a save can never smuggle in an illegal fit
    // (data changed, item deleted, slot mask tightened). Unknown/illegal lines are
    // dropped. Clears first.
    void deserialize(std::string_view text,
                     const AttachSpec* (*resolve)(void*, const std::string&), void* user,
                     uint8_t (*maskOf)(void*, int));

private:
    std::vector<WeaponLoadout> m_w;
};

// Headless self-test (--test-attachments). Slot-mask enforcement, the stat fold,
// composition with the skilltree layer (computed independently and compared), the
// tradeoffs actually biting, the REAL ADS sight-alignment solve (with a NEGATIVE
// CONTROL that proves the probe can fail), and a save round-trip.
bool runAttachmentsSelfTest();

} // namespace x3::game
