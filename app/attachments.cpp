// WEAPON ATTACHMENTS — the data layer + THE FOLD + the ADS sight solve. See attachments.h.
#include "attachments.h"
#include "weapon.h"          // WeaponDef / Arsenal (applyAttachments + the ADS solve live here)
#include "item_db.h"         // --test-attachments resolves specs through the real item DB
#include "skilltree.h"       // --test-attachments asserts composition with the SKILL layer

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Names.
// ---------------------------------------------------------------------------
const char* attachSlotName(AttachSlot s) {
    switch (s) {
        case AttachSlot::Optic:       return "optic";
        case AttachSlot::Barrel:      return "barrel";
        case AttachSlot::Magazine:    return "magazine";
        case AttachSlot::Underbarrel: return "underbarrel";
        case AttachSlot::Coating:     return "coating";
        default:                      return "?";
    }
}
AttachSlot attachSlotFromName(std::string_view s) {
    if (s == "optic")       return AttachSlot::Optic;
    if (s == "barrel")      return AttachSlot::Barrel;
    if (s == "magazine")    return AttachSlot::Magazine;
    if (s == "underbarrel") return AttachSlot::Underbarrel;
    return AttachSlot::Coating;
}
AttachPart attachPartFromName(std::string_view s) {
    if (s == "suppressor")   return AttachPart::Suppressor;
    if (s == "compensator")  return AttachPart::Compensator;
    if (s == "heavy_barrel") return AttachPart::HeavyBarrel;
    if (s == "ext_mag")      return AttachPart::ExtMag;
    if (s == "fast_mag")     return AttachPart::FastMag;
    if (s == "reflex")       return AttachPart::Reflex;
    if (s == "scope")        return AttachPart::Scope;
    if (s == "laser")        return AttachPart::Laser;
    if (s == "grip")         return AttachPart::Grip;
    if (s == "cell")         return AttachPart::Cell;
    return AttachPart::None;
}

int  WeaponLoadout::fittedCount() const {
    int n = 0;
    for (int i = 0; i < kAttachSlotCount; ++i) if (slots[i].valid) ++n;
    return n;
}
void WeaponLoadout::clear() { for (int i = 0; i < kAttachSlotCount; ++i) slots[i] = AttachSpec{}; }

// ---------------------------------------------------------------------------
// Loadouts.
// ---------------------------------------------------------------------------
static WeaponLoadout g_emptyLoadout;

void Loadouts::resize(int weapons) {
    if (weapons < 0) weapons = 0;
    m_w.resize((size_t)weapons);
}
const WeaponLoadout& Loadouts::at(int w) const {
    if (w < 0 || w >= (int)m_w.size()) return g_emptyLoadout;
    return m_w[(size_t)w];
}
WeaponLoadout& Loadouts::mut(int w) {
    if (w < 0) w = 0;
    if (w >= (int)m_w.size()) m_w.resize((size_t)w + 1);
    return m_w[(size_t)w];
}

// THE SLOT-MASK RULE.
bool Loadouts::canFit(uint8_t weaponSlotMask, const AttachSpec& a) {
    if (!a.valid) return false;
    if ((int)a.slot < 0 || (int)a.slot >= kAttachSlotCount) return false;
    return (weaponSlotMask & slotBit(a.slot)) != 0;
}

bool Loadouts::fit(int weapon, uint8_t weaponSlotMask, const AttachSpec& a, std::string* displaced) {
    if (displaced) displaced->clear();
    if (weapon < 0) return false;
    if (!canFit(weaponSlotMask, a)) return false;
    WeaponLoadout& L = mut(weapon);
    AttachSpec& cell = L.slots[(int)a.slot];
    if (cell.valid && displaced) *displaced = cell.id;
    cell = a;
    cell.valid = true;
    return true;
}

std::string Loadouts::unfit(int weapon, AttachSlot s) {
    if (weapon < 0 || weapon >= (int)m_w.size()) return {};
    if ((int)s < 0 || (int)s >= kAttachSlotCount) return {};
    AttachSpec& cell = m_w[(size_t)weapon].slots[(int)s];
    if (!cell.valid) return {};
    const std::string id = cell.id;
    cell = AttachSpec{};
    return id;
}

std::string Loadouts::serialize() const {
    std::ostringstream o;
    for (size_t w = 0; w < m_w.size(); ++w)
        for (int s = 0; s < kAttachSlotCount; ++s) {
            const AttachSpec& a = m_w[w].slots[s];
            if (a.valid && !a.id.empty())
                o << "attach " << w << ' ' << attachSlotName((AttachSlot)s) << ' ' << a.id << '\n';
        }
    return o.str();
}

void Loadouts::deserialize(std::string_view text,
                           const AttachSpec* (*resolve)(void*, const std::string&), void* user,
                           uint8_t (*maskOf)(void*, int)) {
    for (WeaponLoadout& L : m_w) L.clear();
    if (!resolve || !maskOf) return;
    std::istringstream in{ std::string(text) };
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("attach ", 0) != 0) continue;
        std::istringstream ls(line.substr(7));
        int w = -1; std::string slotName, id;
        if (!(ls >> w >> slotName >> id)) continue;
        const AttachSpec* a = resolve(user, id);
        if (!a || !a->valid) continue;                          // item vanished from the data
        if (a->slot != attachSlotFromName(slotName)) continue;  // slot moved: drop the stale fit
        // Re-check the mask: a save can never smuggle in an illegal fit.
        (void)fit(w, maskOf(user, w), *a);
    }
}

// ===========================================================================
// THE FOLD — the ONE place a weapon's attachment-effective stats are computed.
// ===========================================================================
WeaponDef applyAttachments(const WeaponDef& base, const WeaponLoadout& L) {
    WeaponDef d = base;   // vm* / muzzle / FX / audio / kind all ride through untouched

    // Multiplicative accumulators over the fitted slots: prod(1 + fraction).
    float dm = 1, sp = 1, rc = 1, rg = 1, mg = 1, rl = 1, ac = 1, ns = 1, fl = 1, hd = 1;
    float crit = 0.0f;                    // FLAT: additive
    int   dtype = -1;                     // last coating wins (only Coating sets it)

    for (int i = 0; i < kAttachSlotCount; ++i) {
        const AttachSpec& a = L.slots[i];
        if (!a.valid) continue;
        const AttachStats& s = a.stats;
        dm *= (1.0f + s.damageMult);
        sp *= (1.0f + s.spreadMult);
        rc *= (1.0f + s.recoilMult);
        rg *= (1.0f + s.rangeMult);
        mg *= (1.0f + s.magMult);
        rl *= (1.0f + s.reloadMult);
        ac *= (1.0f + s.ammoCapMult);
        ns *= (1.0f + s.noiseMult);
        fl *= (1.0f + s.flashScale);
        hd *= (1.0f + s.handlingMult);
        crit += s.critChance;
        if (s.damageType >= 0) dtype = s.damageType;
    }

    auto iround = [](float v) { return (int)(v + (v < 0 ? -0.5f : 0.5f)); };
    d.damage      = std::max(1, iround((float)base.damage * dm));
    d.spreadDeg   = std::max(0.0f, base.spreadDeg * sp);
    d.recoilDeg   = std::max(0.0f, base.recoilDeg * rc);
    d.range       = std::max(1.0f, base.range * rg);
    d.magSize     = std::max(1, iround((float)base.magSize * mg));
    d.reloadTime  = std::max(0.15f, base.reloadTime * rl);
    d.reserveAmmo = std::max(0, iround((float)base.reserveAmmo * ac));
    d.critBonus   = base.critBonus + crit;
    d.noiseMult   = std::max(0.0f, base.noiseMult * ns);
    d.flashScale  = std::max(0.05f, base.flashScale * fl);
    d.handlingMult= std::max(0.25f, base.handlingMult * hd);
    if (dtype >= 0 && dtype < (int)x3::DamageType::Count) d.type = (x3::DamageType)dtype;
    return d;
}

// ===========================================================================
// SIGHT / MOUNT GEOMETRY — derived from the MEASURED per-weapon muzzle (346f5e7).
//
// Every gun's barrel tip is already known in its GLB's scene space (WeaponDef::
// vmMuzzle). The barrel RUN (|vmMuzzle|) is that gun's own length scale, so every
// mount point below is expressed as a FRACTION of it — nothing is hardcoded per
// weapon, and a 4.4 m shotgun and a 0.9 m pistol both mount correctly.
//
// THIS IS THE SINGLE SOURCE for the optic's lens centre. The drawn glass
// (attach_view.cpp) and the ADS alignment solve BOTH call it. If they ever used
// two different points, the reticle would lie about where the bullet goes — the
// exact class of bug this project keeps re-making.
// ===========================================================================
float attachBarrelRun(const WeaponDef& d) {
    const float r = std::sqrt(d.vmMuzzle.x * d.vmMuzzle.x +
                              d.vmMuzzle.y * d.vmMuzzle.y +
                              d.vmMuzzle.z * d.vmMuzzle.z);
    return (r > 1e-4f) ? r : 1.0f;
}

// THE MOUNT. Two measured sources, not one:
//   * vmMuzzle  — the barrel tip (346f5e7). Exact, and the ONLY thing a barrel mod needs.
//   * vmBounds  — the gun's real box (loadViewmodels). |vmMuzzle| is a LENGTH; it says
//                 nothing about how WIDE or TALL a gun is. Mounting an optic/magazine/
//                 side cell off the length alone put parts IN THE AIR beside the weapon
//                 (caught in the first capture). "On top of" / "under" / "on the flank"
//                 are box questions, so they are answered by the box.
// Headless (no device -> no bounds) falls back to the old barrel-run guess so the pure
// logic tests still run.
x3::phys::Vec3 attachMountLocal(const WeaponDef& d, const AttachSpec& a) {
    const float s = attachBarrelRun(d);
    const x3::phys::Vec3& m = d.vmMuzzle;
    if (a.slot == AttachSlot::Barrel) return m;         // measured tip: nothing to guess
    if (a.slot == AttachSlot::Optic)  return attachSightLocal(d, a);

    // EVERYTHING ON A GUN IS ARRANGED AROUND THE BORE. The bore is the line through
    // vmMuzzle — measured, per weapon. So Y is BORE-relative (a small offset above or
    // below the barrel), NOT box-relative: anchoring on the box's extremes hangs parts
    // off the gun's tallest/lowest point, which for a pistol is the rear of the slide
    // and the base of the grip — nowhere near the rail or the magwell. (The first
    // debug-tinted capture showed exactly that: an optic and a magazine floating in
    // mid-air beside the weapon.)
    //
    // The BOX still owns what only it knows: the centre-line (X) and the flank (max X).
    // ...and Z is measured FROM THE MODEL ORIGIN toward the muzzle, not from the box's
    // rear face. Every weapon GLB in this game is authored centred (the box is symmetric
    // in Z: the pistol runs -0.92..0.90, the shotgun -2.20..2.20), so the ORIGIN is
    // mid-gun — the receiver — and the muzzle is one barrel-run ahead of it. A fraction
    // of the *muzzle distance* therefore reads directly as "how far down the barrel":
    // 0 = on the receiver, 1 = at the tip. (Taking 30% of the muzzle distance and calling
    // it "the receiver" is what stacked the optic and the magazine around the BARREL in
    // the second debug capture.)
    const float cx = d.vmBounds ? (d.vmMin.x + d.vmMax.x) * 0.5f : m.x;
    const float fx = d.vmBounds ? d.vmMax.x : (m.x + 0.075f * s);
    switch (a.slot) {
        case AttachSlot::Magazine:    // hangs UNDER the bore, BEHIND the receiver (the magwell)
            return { cx, m.y - 0.13f * s, m.z * -0.20f };
        case AttachSlot::Underbarrel: // clamped UNDER the barrel, ahead of the receiver
            return { cx, m.y - 0.085f * s, m.z * 0.42f };
        case AttachSlot::Coating:     // an energy cell ON THE FLANK of the receiver
            return { fx, m.y - 0.03f * s, m.z * -0.08f };
        default: return m;
    }
}

// THE SIGHT — the optic's LENS CENTRE. The single point the drawn glass AND the ADS
// alignment solve both use. Sits ON THE GUN'S TOP, over the receiver, on the barrel's
// own centre-line (so the sight line is not skewed off-axis).
x3::phys::Vec3 attachSightLocal(const WeaponDef& d, const AttachSpec& a) {
    const x3::phys::Vec3& m = d.vmMuzzle;
    const float rise  = a.optic.isOptic ? a.optic.sightRise  : 0.115f;
    const float along = a.optic.isOptic ? a.optic.sightAlong : 0.30f;
    const float s   = attachBarrelRun(d);
    const float cx  = d.vmBounds ? (d.vmMin.x + d.vmMax.x) * 0.5f : m.x;
    // BORE-RELATIVE, and then clamped to sit just PROUD OF THE GUN'S TOP FACE — so the
    // glass is above every part of the weapon (nothing pokes through it) without floating
    // (the sight's own rail base reaches back down into the receiver).
    float y = m.y + rise * s;
    if (d.vmBounds) {
        const float minY = d.vmMax.y + 0.030f * s;   // just clear of the tallest geometry
        if (y < minY) y = minY;
    }
    return { cx, y, m.z * along };
}

// ---------------------------------------------------------------------------
// THE ADS SOLVE. Closed form, exact.
//
// The FP viewmodel world frame is (see Arsenal::currentViewmodelFrame):
//     pos   = eye + fwd*F + right*R - up*D
//     world = pos + (bx*x + by*y + bz*z) * scale
// with (bx,by,bz) depending ONLY on the yaw/pitch/roll offsets — not on F/R/D.
//
// Let V = (bx*sx + by*sy + bz*sz) * scale be the sight point's offset from `pos`.
// The sight lies ON the camera axis iff (world - eye) has ZERO component along
// `right` and along `up`:
//     R + dot(V, right) = 0   ->   R = -dot(V, right)
//    -D + dot(V, up)    = 0   ->   D =  dot(V, up)
// F is free (it only slides the gun along the axis) — we pull it in slightly.
//
// The returned values are DELTAS on top of the weapon's own vmRight/vmDown, i.e.
// exactly what drawCurrentViewmodel()/currentViewmodelFrame() take as extraRight/
// extraDown. So the gun is genuinely ALIGNED — it is not an FOV lerp with the gun
// sitting off-axis.
// ---------------------------------------------------------------------------
bool Arsenal::solveAdsOffsets(const x3::phys::Vec3& sightLocal, float yaw, float pitch,
                              float extraFwd, float& outRight, float& outDown) const {
    outRight = 0.0f; outDown = 0.0f;
    if (m_sel < 0 || m_sel >= (int)m_defs.size()) return false;
    const WeaponDef& d = m_defs[(size_t)m_sel];

    // Basis at the HIP pose (translation offsets don't affect bx/by/bz).
    const VmFrame f = currentViewmodelFrame(0, 0, 0, yaw, pitch, 0, 0, 0, extraFwd, 0, 0);

    const x3::phys::Vec3 V{
        (f.bx.x * sightLocal.x + f.by.x * sightLocal.y + f.bz.x * sightLocal.z) * f.scale,
        (f.bx.y * sightLocal.x + f.by.y * sightLocal.y + f.bz.y * sightLocal.z) * f.scale,
        (f.bx.z * sightLocal.x + f.by.z * sightLocal.y + f.bz.z * sightLocal.z) * f.scale };

    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    const x3::phys::Vec3 right{ -sy, 0.0f, cy };
    const x3::phys::Vec3 up{ right.y * forward.z - right.z * forward.y,
                             right.z * forward.x - right.x * forward.z,
                             right.x * forward.y - right.y * forward.x };
    const float vR = V.x * right.x + V.y * right.y + V.z * right.z;
    const float vU = V.x * up.x    + V.y * up.y    + V.z * up.z;

    outRight = -vR - d.vmRight;   // DELTA on the weapon's own vmRight
    outDown  =  vU - d.vmDown;    // DELTA on the weapon's own vmDown
    return true;
}

x3::phys::Vec3 Arsenal::sightWorld(const x3::phys::Vec3& sightLocal,
                                   float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                                   float extraFwd, float extraRight, float extraDown) const {
    const VmFrame f = currentViewmodelFrame(eyeX, eyeY, eyeZ, yaw, pitch, 0, 0, 0,
                                            extraFwd, extraRight, extraDown);
    return { f.pos.x + (f.bx.x * sightLocal.x + f.by.x * sightLocal.y + f.bz.x * sightLocal.z) * f.scale,
             f.pos.y + (f.bx.y * sightLocal.x + f.by.y * sightLocal.y + f.bz.y * sightLocal.z) * f.scale,
             f.pos.z + (f.bx.z * sightLocal.x + f.by.z * sightLocal.y + f.bz.z * sightLocal.z) * f.scale };
}

const AttachSpec* Arsenal::currentOptic() const {
    return m_loadouts.at(m_sel).optic();
}

// ===========================================================================
// --test-attachments
// ===========================================================================
namespace {
int g_apass = 0, g_afail = 0;
void acheck(bool ok, const std::string& what) {
    if (ok) { ++g_apass; x3::logInfo("  PASS A" + std::to_string(g_apass + g_afail) + ": " + what); }
    else    { ++g_afail; x3::logError("  FAIL A" + std::to_string(g_apass + g_afail) + ": " + what); }
}
bool nearly(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// PERPENDICULAR DISTANCE from `p` to the ray (eye + t*dir). This is THE probe: it
// measures whether the thing you are looking THROUGH sits on the line the bullet
// travels. A scoped sight must be ~0 from it; a hip-held sight must not be.
float rayPointDist(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir, const x3::phys::Vec3& p) {
    const x3::phys::Vec3 v{ p.x - eye.x, p.y - eye.y, p.z - eye.z };
    const float t = v.x * dir.x + v.y * dir.y + v.z * dir.z;
    const x3::phys::Vec3 c{ v.x - dir.x * t, v.y - dir.y * t, v.z - dir.z * t };
    return std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
}
} // namespace

bool runAttachmentsSelfTest() {
    g_apass = g_afail = 0;
    x3::logInfo("--test-attachments: weapon attachments (slots / fold / composition / ADS / save)");

    ItemDb db;
    db.load(itemsJsonPath());   // load-or-bake; the baked table carries the full attach set

    auto spec = [&](const char* id) -> const AttachSpec* {
        const ItemDef* d = db.find(id);
        return (d && d->attach.valid) ? &d->attach : nullptr;
    };

    // ---- A1: the data is there and every attachment declares a real slot ----
    const char* kAll[] = { "att_suppressor", "att_compensator", "att_heavy_barrel",
                           "att_ext_mag", "att_fast_mag", "att_reflex", "att_acog",
                           "att_scope", "att_laser", "att_grip",
                           "att_salvari_coating", "att_kinetic_coating" };
    bool allThere = true;
    for (const char* id : kAll) if (!spec(id)) { allThere = false; x3::logError(std::string("  missing attachment: ") + id); }
    acheck(allThere, "item DB carries all 12 attachments with a valid attach spec");

    Arsenal ar;
    const int wPistol   = ar.indexOf("pistol");
    const int wShotgun  = ar.indexOf("shotgun");
    const int wPlasma   = ar.indexOf("plasma");
    acheck(wPistol >= 0 && wShotgun >= 0 && wPlasma >= 0, "roster resolves pistol / shotgun / plasma");

    // ---- A2: THE SLOT MASK IS REAL — an optic cannot go in a barrel slot ----
    // (a) The pistol accepts an optic but NOT an underbarrel.
    const bool optOk   = ar.fitAttachment(wPistol, *spec("att_reflex"));
    const bool underNo = ar.fitAttachment(wPistol, *spec("att_grip"));
    acheck(optOk && !underNo, "slot mask: pistol takes an Optic, REFUSES an Underbarrel");
    // (b) The BFG-class plasma is Coating-only: an optic and a barrel mod both bounce.
    const bool bfgCoat = ar.fitAttachment(wPlasma, *spec("att_salvari_coating"));
    const bool bfgOpt  = ar.fitAttachment(wPlasma, *spec("att_reflex"));
    const bool bfgBar  = ar.fitAttachment(wPlasma, *spec("att_suppressor"));
    acheck(bfgCoat && !bfgOpt && !bfgBar, "slot mask: the BFG is Coating-only (optic + barrel refused)");
    // (c) A barrel-slot item can never land in the Optic slot: the SLOT belongs to
    //     the ATTACHMENT, so a suppressor fit only ever occupies Barrel.
    ar.fitAttachment(wShotgun, *spec("att_suppressor"));
    acheck(ar.loadouts().at(wShotgun).has(AttachSlot::Barrel) &&
           !ar.loadouts().at(wShotgun).has(AttachSlot::Optic),
           "an attachment lands ONLY in its own slot (suppressor -> Barrel, never Optic)");

    // ---- A3: THE TRADEOFF IS REAL — a suppressor actually LOWERS damage ----
    {
        Arsenal a2;
        const WeaponDef base = a2.baseDef(wShotgun);
        a2.select(wShotgun);
        a2.fitAttachment(wShotgun, *spec("att_suppressor"));
        const WeaponDef& eff = a2.current();
        const int expect = (int)((float)base.damage * (1.0f - 0.15f) + 0.5f);
        acheck(eff.damage == expect && eff.damage < base.damage,
               "suppressor: damage " + std::to_string(base.damage) + " -> " +
               std::to_string(eff.damage) + " (REALLY lower)");
        acheck(eff.noiseMult < 0.35f && eff.flashScale < 0.5f,
               "suppressor: hearing radius x" + std::to_string(eff.noiseMult) +
               " and flash x" + std::to_string(eff.flashScale) + " (quieter + dimmer)");
    }
    // ---- A4: every attachment GIVES and TAKES (no free lunch anywhere) ----
    {
        bool everyoneCosts = true;
        for (const char* id : kAll) {
            const AttachStats& s = spec(id)->stats;
            const float ups[] = { s.damageMult, -s.spreadMult, -s.recoilMult, s.rangeMult,
                                  s.magMult, -s.reloadMult, s.ammoCapMult, -s.noiseMult,
                                  s.handlingMult, s.critChance };
            bool anyUp = false, anyDown = false;
            for (float v : ups) { if (v > 1e-4f) anyUp = true; if (v < -1e-4f) anyDown = true; }
            if (s.flashScale > 1e-4f) anyDown = true;        // a bigger flash IS a cost
            if (s.damageType >= 0)    anyUp   = true;        // a type switch IS the upside
            if (!(anyUp && anyDown)) {
                everyoneCosts = false;
                x3::logError(std::string("  free lunch: ") + id + " has no real downside");
            }
        }
        acheck(everyoneCosts, "every attachment has BOTH an upside and a downside");
    }
    // ---- A5: extended vs fast magazine trade opposite ways ----
    {
        Arsenal a2; const WeaponDef base = a2.baseDef(wShotgun);
        a2.select(wShotgun);
        a2.fitAttachment(wShotgun, *spec("att_suppressor"));  // a DIFFERENT slot: coexists
        a2.fitAttachment(wShotgun, *spec("att_ext_mag"));
        const WeaponDef ext = a2.current();
        a2.fitAttachment(wShotgun, *spec("att_fast_mag"));   // SAME slot: replaces the ext mag
        const WeaponDef fast = a2.current();
        acheck(ext.magSize > base.magSize && ext.reloadTime > base.reloadTime,
               "extended mag: +capacity, -reload speed");
        acheck(fast.magSize < base.magSize && fast.reloadTime < base.reloadTime,
               "fast mag: +reload speed, -capacity");
        acheck(a2.loadouts().at(wShotgun).fittedCount() == 2,
               "same-slot fit REPLACES (barrel + magazine = 2 fitted, not 3)");
    }
    // ---- A6: MULTIPLICATIVE composition across slots (independently computed) ----
    {
        Arsenal a2; a2.select(wShotgun);
        const WeaponDef base = a2.baseDef(wShotgun);
        a2.fitAttachment(wShotgun, *spec("att_heavy_barrel"));    // damage +20%
        a2.fitAttachment(wShotgun, *spec("att_kinetic_coating")); // damage +12%
        const float expectF = (float)base.damage * 1.20f * 1.12f;
        const int   expect  = (int)(expectF + 0.5f);
        acheck(a2.current().damage == expect,
               "two slots compose MULTIPLICATIVELY: " + std::to_string(base.damage) +
               " x1.20 x1.12 = " + std::to_string(expect) + " (got " +
               std::to_string(a2.current().damage) + ")");
        acheck(a2.current().type == x3::DamageType::Kinetic,
               "coating sets the shot's DamageType (Kinetic sheath)");
        a2.fitAttachment(wShotgun, *spec("att_salvari_coating"));
        acheck(a2.current().type == x3::DamageType::Energy,
               "Salvari coating flips the shot to Energy (the ENERGY slot is real)");
    }
    // ---- A7: COMPOSITION WITH THE SKILL LAYER (the two-layer rule) ----
    // Layer 1 = attachments -> effective WeaponDef (int damage). Layer 2 = skills ->
    // PlayerStatMods, applied at the fire site. Computed here INDEPENDENTLY.
    {
        Arsenal a2; a2.select(wPistol);
        const WeaponDef base = a2.baseDef(wPistol);
        a2.fitAttachment(wPistol, *spec("att_heavy_barrel"));   // +20% damage
        const int layer1 = a2.current().damage;
        const int expect1 = (int)((float)base.damage * 1.20f + 0.5f);

        PlayerStatMods mods;              // pretend the skill tree gave +15%
        mods.damageMult = 1.15f;
        mods.critChance = 0.0f;
        uint32_t rng = 12345u;
        const int layer2 = rpgScaleDamage(layer1, mods, rng);
        const int expect2 = (int)((float)expect1 * 1.15f + 0.5f);

        acheck(layer1 == expect1 && layer2 == expect2,
               "stacking: base " + std::to_string(base.damage) + " -> attach x1.20 = " +
               std::to_string(layer1) + " -> skill x1.15 = " + std::to_string(layer2) +
               " (multiplicative, independent layers, quantized once per layer)");

        // Crit is the ADDITIVE channel: skill crit + attachment crit.
        PlayerStatMods m2; m2.critChance = 0.08f;               // a skill
        const float combined = m2.critChance + a2.current().critBonus;   // + reflex/scope etc.
        a2.fitAttachment(wPistol, *spec("att_reflex"));         // +0.05 crit
        const float combined2 = m2.critChance + a2.current().critBonus;
        acheck(nearly(combined2 - combined, 0.05f, 1e-5f),
               "crit composes ADDITIVELY across the skill + attachment layers (+0.05)");
    }
    // ---- A8: NEGATIVE CONTROL for the fold — an EMPTY loadout must be a no-op ----
    {
        Arsenal a2; a2.select(wPistol);
        const WeaponDef& b = a2.baseDef(wPistol);
        const WeaponDef& e = a2.current();
        const bool identity = e.damage == b.damage && nearly(e.spreadDeg, b.spreadDeg) &&
                              e.magSize == b.magSize && nearly(e.reloadTime, b.reloadTime) &&
                              nearly(e.vmMuzzle.z, b.vmMuzzle.z) && nearly(e.noiseMult, 1.0f);
        acheck(identity, "empty loadout == base def EXACTLY (the fold is a no-op, muzzle intact)");
    }

    // =======================================================================
    // A9..A12 — THE SCOPE. The optic is a FEATURE, not a number.
    // =======================================================================
    {
        Arsenal a2; a2.select(wPistol);
        a2.fitAttachment(wPistol, *spec("att_scope"));
        const AttachSpec* op = a2.currentOptic();
        const x3::phys::Vec3 sight = attachSightLocal(a2.current(), *op);

        const float eyeX = 3.0f, eyeY = 1.7f, eyeZ = -4.0f;
        const float yaw = 0.7f, pitch = -0.18f;               // an arbitrary, awkward pose
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float cy = std::cos(yaw),   sy = std::sin(yaw);
        // The FIRE direction: exactly what app_run's fire site uses (camera forward)
        // and exactly what the reticle (screen centre) points at.
        const x3::phys::Vec3 dir{ cp * cy, sp, cp * sy };
        const x3::phys::Vec3 eye{ eyeX, eyeY, eyeZ };

        // --- A9: SCOPED — the lens centre sits ON the line the bullet travels. ---
        const float adsFwd = -0.15f;
        float aR = 0, aD = 0;
        a2.solveAdsOffsets(sight, yaw, pitch, adsFwd, aR, aD);
        const x3::phys::Vec3 scoped = a2.sightWorld(sight, eyeX, eyeY, eyeZ, yaw, pitch, adsFwd, aR, aD);
        const float dScoped = rayPointDist(eye, dir, scoped);
        acheck(dScoped < 1e-3f,
               "ADS: the SIGHT LINE and the SHOT agree — lens centre is " +
               std::to_string(dScoped) + " m off the fire ray (< 1 mm)");

        // --- A10: NEGATIVE CONTROL. The SAME probe on the HIP pose must FAIL. ---
        // (Deliberately misalign the sight: use the weapon's hip offsets. If this
        //  came back "aligned" the probe would be measuring nothing at all.)
        const x3::phys::Vec3 hip = a2.sightWorld(sight, eyeX, eyeY, eyeZ, yaw, pitch, 0, 0, 0);
        const float dHip = rayPointDist(eye, dir, hip);
        acheck(dHip > 0.02f,
               "NEGATIVE CONTROL: the hip-held sight is " + std::to_string(dHip) +
               " m OFF the fire ray — the probe CAN fail (so A9 means something)");
        // ...and a second control: nudge the solved offset by 1 cm and watch A9's
        // assertion break. A test that cannot fail is worthless.
        const x3::phys::Vec3 bent = a2.sightWorld(sight, eyeX, eyeY, eyeZ, yaw, pitch,
                                                  adsFwd, aR + 0.01f, aD);
        acheck(rayPointDist(eye, dir, bent) > 0.005f,
               "NEGATIVE CONTROL: a 1 cm misalignment of the solve trips the same probe");

        // --- A11: REAL magnification + REAL handling cost, and optics DIFFER. ---
        const AttachSpec* reflex = spec("att_reflex");
        const AttachSpec* scope  = spec("att_scope");
        const AttachSpec* acog   = spec("att_acog");
        acheck(scope->optic.adsFovDeg < acog->optic.adsFovDeg &&
               acog->optic.adsFovDeg  < reflex->optic.adsFovDeg &&
               reflex->optic.adsFovDeg < kBaseFovDeg,
               "FOV really narrows, and by different amounts: scope " +
               std::to_string((int)scope->optic.adsFovDeg) + " < acog " +
               std::to_string((int)acog->optic.adsFovDeg) + " < reflex " +
               std::to_string((int)reflex->optic.adsFovDeg) + " < base 60");
        acheck(scope->optic.adsTime > acog->optic.adsTime &&
               acog->optic.adsTime  > reflex->optic.adsTime,
               "ADS speed differs: the reflex snaps up, the sniper scope is slow");
        acheck(scope->stats.handlingMult < reflex->stats.handlingMult &&
               scope->stats.handlingMult < 0.0f,
               "handling penalty is real and heaviest on the sniper scope");
        acheck(scope->optic.fullScope && !reflex->optic.fullScope,
               "the sniper scope draws a full scope picture; the reflex stays open-sighted");

        // --- A12: the handling penalty reaches the EFFECTIVE weapon (turn speed). ---
        Arsenal a3; a3.select(wPistol);
        const float hipHandling = a3.current().handlingMult;
        a3.fitAttachment(wPistol, *spec("att_scope"));
        acheck(nearly(hipHandling, 1.0f) && a3.current().handlingMult < 0.7f,
               "the scope's handling cost lands on the effective weapon (turn x" +
               std::to_string(a3.current().handlingMult) + ")");
    }

    // ---- A13: PERSISTENCE round-trips (and cannot smuggle an illegal fit) ----
    {
        Arsenal a2;
        a2.fitAttachment(wPistol,  *spec("att_suppressor"));
        a2.fitAttachment(wPistol,  *spec("att_reflex"));
        a2.fitAttachment(wShotgun, *spec("att_ext_mag"));
        const std::string text = a2.loadouts().serialize();

        Arsenal a3;   // a fresh game: nothing fitted
        acheck(a3.loadouts().at(wPistol).fittedCount() == 0, "a fresh Arsenal starts bare");
        a3.restoreAttachments(text, db);
        const WeaponLoadout& L = a3.loadouts().at(wPistol);
        acheck(L.has(AttachSlot::Barrel) && L.has(AttachSlot::Optic) &&
               L.slots[(int)AttachSlot::Barrel].id == "att_suppressor" &&
               a3.loadouts().at(wShotgun).has(AttachSlot::Magazine),
               "save round-trip: fitted attachments survive serialize -> deserialize");
        a3.select(wPistol);
        a2.select(wPistol);
        acheck(a3.current().damage == a2.current().damage &&
               nearly(a3.current().noiseMult, a2.current().noiseMult),
               "restored loadout reproduces the SAME effective stats (stats are re-resolved, not stored)");

        // A hand-edited save that puts a grip on the pistol (no underbarrel) is DROPPED.
        Arsenal a4;
        a4.restoreAttachments("attach " + std::to_string(wPistol) + " underbarrel att_grip\n", db);
        acheck(a4.loadouts().at(wPistol).fittedCount() == 0,
               "a save carrying an ILLEGAL fit (grip on the pistol) is refused on load");
    }

    x3::logInfo("--test-attachments: " + std::to_string(g_apass) + " passed, " +
                std::to_string(g_afail) + " failed");
    return g_afail == 0;
}

} // namespace x3::game
