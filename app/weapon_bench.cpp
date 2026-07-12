// THE WEAPON BENCH — see weapon_bench.h.
#include "weapon_bench.h"
#include "mesh_prims.h"

#include <cctype>
#include <cstdio>

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>

namespace x3::game {

// ===========================================================================
// bakeBench — a new BAKER on the ONE holo platform (no second holo, ever).
// Ink: BLUE structure / GREEN gain / ORANGE cost. Never cyan.
// Readability beats decoration: line-art keeps to the margins, every text row
// gets its own quietBand, and the type is sized to be read at [E] range.
// ===========================================================================
std::vector<uint8_t> bakeBench(uint32_t n, const std::string& weapon,
                               const std::vector<BenchRow>& slotRows,
                               const std::vector<BenchRow>& statRows,
                               int sel, const std::string& hint, float aspect) {
    using namespace holo;
    Canvas c(n);
    blackGlassBase(c);
    const float fn = (float)n;
    auto P = [&](float f) { return f * fn; };
    const float xs = (aspect > 0.001f) ? 1.0f / aspect : 1.0f;
    const float th = std::max(1.4f, fn / 512.0f);

    // --- Structure, kept to the MARGINS. ---
    rectFrame(c, P(0.045f), P(0.045f), P(0.955f), P(0.955f), kBlue, 0.55f, th * 1.4f);
    line(c, P(0.06f), P(0.155f), P(0.94f), P(0.155f), kBlueHi, 0.75f, th);
    hexagon(c, P(0.912f), P(0.100f), P(0.028f), kBlue, 0.60f, th);

    // --- Title + the weapon under the vice (row 0 = the weapon selector). ---
    quietBand(c, P(0.06f), P(0.048f), P(0.94f), P(0.152f), 0.08f, P(0.006f));
    drawText(c, "WEAPON BENCH", P(0.078f), P(0.050f), P(0.042f), kBlueHi, 1.0f, xs);
    {
        const bool onSel = (sel == 0);
        const std::string w = "< " + weapon + " >";
        float px = P(0.052f);
        const float wd = textWidth(w, px, xs);
        if (wd > P(0.50f) && wd > 1.0f) px *= P(0.50f) / wd;
        drawText(c, w, P(0.082f), P(0.100f), px, onSel ? kGreen : kBlue, 1.0f, xs);
        if (onSel) rectFrame(c, P(0.068f), P(0.096f), P(0.62f), P(0.150f), kGreen, 0.7f, th);
    }

    // --- The FIVE SLOTS. ---
    const float top = P(0.175f), bot = P(0.560f);
    const size_t rows = std::max<size_t>(1, slotRows.size());
    const float rowH = (bot - top) / (float)rows;
    for (size_t i = 0; i < slotRows.size(); ++i) {
        const BenchRow& r = slotRows[i];
        const float y0 = top + rowH * (float)i, y1 = y0 + rowH * 0.86f;
        const bool onSel = ((int)i + 1 == sel);
        quietBand(c, P(0.065f), y0, P(0.935f), y1, 0.09f, P(0.005f));
        //  fitted = GREEN · not accepted = ORANGE · empty/pending = BLUE
        Ink k = kBlue;
        if (r.state == 1) k = kGreen;
        else if (r.state == 2) k = kOrange;
        else if (r.state == 3) k = kAmber;
        if (onSel) {
            rectFrame(c, P(0.065f), y0, P(0.935f), y1, k, 0.80f, th);
            rectFill(c, P(0.075f), y0 + rowH * 0.28f, P(0.098f), y0 + rowH * 0.56f, k, 1.0f);
        }
        const float tpx = std::min(rowH * 0.52f, P(0.040f));
        drawText(c, r.label, P(0.115f), y0 + rowH * 0.12f, tpx, kBlue, 0.85f, xs);
        float vx = P(0.040f);
        const float vw = textWidth(r.value, vx, xs);
        if (vw > P(0.50f) && vw > 1.0f) vx *= P(0.50f) / vw;
        drawText(c, r.value, P(0.400f), y0 + rowH * 0.12f, vx, k, 1.0f, xs);
    }

    // --- The LIVE STAT READOUT: base -> effective. GREEN = gain, ORANGE = cost. ---
    line(c, P(0.06f), P(0.575f), P(0.94f), P(0.575f), kBlue, 0.45f, th * 0.8f);
    drawText(c, "EFFECTIVE", P(0.075f), P(0.585f), P(0.034f), kBlueHi, 0.9f, xs);
    // TWO COLUMNS. Eight one-line rows down a 26%-tall strip made the type unreadable
    // at [E] range (measured off the first capture). Two columns of three doubles the
    // row height, and the type is sized to the ROW, not to a fixed fraction.
    const float sTop = P(0.630f), sBot = P(0.895f);
    const size_t srows = statRows.size();
    const size_t perCol = (srows + 1) / 2;
    const float sH = (sBot - sTop) / (float)std::max<size_t>((size_t)1, perCol);
    for (size_t i = 0; i < srows; ++i) {
        const BenchRow& r = statRows[i];
        const bool right = (i >= perCol);
        const float col0 = right ? 0.510f : 0.065f;
        const float col1 = right ? 0.935f : 0.490f;
        const float y0 = sTop + sH * (float)(right ? (i - perCol) : i);
        quietBand(c, P(col0), y0, P(col1), y0 + sH * 0.90f, 0.08f, P(0.004f));
        const Ink k = (r.state == 1) ? kGreen : (r.state == 2) ? kOrange : kBlue;
        const float tpx = std::min(sH * 0.70f, P(0.046f));
        drawText(c, r.label, P(col0 + 0.020f), y0, tpx, kBlue, 0.85f, xs);
        float vx = tpx;
        const float vw = textWidth(r.value, vx, xs);
        const float room = P(col1 - col0 - 0.185f);
        if (vw > room && vw > 1.0f) vx *= room / vw;
        drawText(c, r.value, P(col0 + 0.185f), y0, vx, k, 1.0f, xs);
    }

    // --- The caption. ---
    if (!hint.empty()) {
        quietBand(c, P(0.06f), P(0.905f), P(0.94f), P(0.950f), 0.08f, P(0.005f));
        float hp = P(0.030f);
        const float hw = textWidth(hint, hp, xs);
        if (hw > P(0.86f) && hw > 1.0f) hp *= P(0.86f) / hw;
        drawText(c, hint, P(0.075f), P(0.908f), hp, kAmber, 1.0f, xs);
    }
    return finish(c);
}

// ===========================================================================
// The fixture.
// ===========================================================================
void WeaponBench::build(Scene& scene, x3::rhi::IRenderDevice& device,
                        x3::phys::IPhysicsWorld* physics,
                        const x3::phys::Vec3& pos, float yaw, float ceilingY, uint32_t roomId) {
    if (m_built) return;

    const float cy = std::cos(yaw), sy = std::sin(yaw);
    // Bench-local (right, up, fwd) -> world. The panel faces the same way.
    auto place = [&](float lx, float ly, float lz) {
        return x3::phys::Vec3{ pos.x + lx * cy + lz * sy,
                               pos.y + ly,
                               pos.z - lx * sy + lz * cy };
    };
    auto addProp = [&](const x3::prims::PrimMesh& p, const x3::phys::Vec3& w,
                       float r, float g, float b, float metal, float rough, bool collide) {
        Entity e;
        e.mesh = device.createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                   p.index.data(), (uint32_t)p.index.size());
        m_meshes.push_back(e.mesh);
        // Yaw the prop with the bench.
        e.transform[0] = cy;  e.transform[2] = -sy;
        e.transform[8] = sy;  e.transform[10] = cy;
        e.transform[5] = 1.0f; e.transform[15] = 1.0f;
        e.transform[12] = w.x; e.transform[13] = w.y; e.transform[14] = w.z;
        // HONEST albedo — a workbench is dark steel, not a white slab.
        e.baseColor[0] = r; e.baseColor[1] = g; e.baseColor[2] = b; e.baseColor[3] = 1.0f;
        (void)metal; (void)rough;
        e.tag = (uint32_t)Tag::Prop;
        e.visible = true;
        e.roomId = roomId;
        m_ents.push_back(scene.add(e));
        if (collide && physics) {
            // A bench you cannot walk through.
            std::vector<float> cv; cv.reserve(p.verts.size() * 3);
            for (const auto& v : p.verts) {
                const float lx = v.pos[0], ly = v.pos[1], lz = v.pos[2];
                cv.push_back(w.x + lx * cy + lz * sy);
                cv.push_back(w.y + ly);
                cv.push_back(w.z - lx * sy + lz * cy);
            }
            physics->addStaticMesh(cv.data(), (uint32_t)p.verts.size(),
                                   p.index.data(), (uint32_t)p.index.size());
        }
    };

    // ---- The bench: a steel top on two plinths, a vice, and a parts tray. ----
    addProp(x3::prims::makeBox(0.95f, 0.045f, 0.42f, 0, 0, 0), place(0, 0.90f, 0),
            0.21f, 0.22f, 0.235f, 0.85f, 0.45f, true);                 // top
    addProp(x3::prims::makeBox(0.10f, 0.44f, 0.34f, 0, 0, 0), place(-0.78f, 0.45f, 0),
            0.13f, 0.135f, 0.14f, 0.80f, 0.55f, true);                 // left plinth
    addProp(x3::prims::makeBox(0.10f, 0.44f, 0.34f, 0, 0, 0), place(0.78f, 0.45f, 0),
            0.13f, 0.135f, 0.14f, 0.80f, 0.55f, true);                 // right plinth
    addProp(x3::prims::makeBox(0.11f, 0.09f, 0.09f, 0, 0, 0), place(-0.30f, 1.03f, 0.02f),
            0.26f, 0.24f, 0.20f, 0.90f, 0.38f, false);                 // the vice
    addProp(x3::prims::makeBox(0.28f, 0.025f, 0.16f, 0, 0, 0), place(0.42f, 0.96f, 0.02f),
            0.17f, 0.18f, 0.185f, 0.60f, 0.60f, false);                // parts tray

    // ---- THE PANEL. A HoloPanel (the ONE implementation), WallFlush so the
    // back-box sits BEHIND the glass — nothing may ever go in front of a screen (L3).
    m_panelPos = place(0.0f, 1.62f, -0.30f);
    m_aspect = 1.30f / 0.98f;
    HoloPanelParams hp;
    hp.pos    = m_panelPos;
    hp.yaw    = yaw;
    hp.width  = 1.30f;
    hp.height = 0.98f;
    hp.ceilingY = (ceilingY > 0.0f) ? ceilingY : (m_panelPos.y + 1.2f);
    hp.frame  = HoloFrame::Pipe;          // the shiny metallic round-pipe frame (canon)
    hp.mount  = HoloMount::WallFlush;
    hp.texN   = m_texN;
    hp.roomId = roomId;
    hp.emissiveStrength = 2.0f;
    hp.shimmerIntensity = 0.6f;           // it is a work surface, not a disco
    hp.glowColor[0] = 0.30f; hp.glowColor[1] = 0.62f; hp.glowColor[2] = 1.50f;
    hp.glowRange = 3.0f;
    const float aspect = m_aspect;
    hp.contentBake = [aspect](uint32_t n) {
        // The IDLE screen (before the player interacts): the bench identifies itself
        // and says what it is for. It must never be a blank pane.
        std::vector<BenchRow> slots = {
            { "OPTIC",       "- EMPTY -", 0 },
            { "BARREL",      "- EMPTY -", 0 },
            { "MAGAZINE",    "- EMPTY -", 0 },
            { "UNDERBARREL", "- EMPTY -", 0 },
            { "COATING",     "- EMPTY -", 0 },
        };
        std::vector<BenchRow> stats = {
            { "STATUS", "BENCH ONLINE", 1 },
            { "VICE",   "READY",        1 },
        };
        return bakeBench(n, "NO WEAPON IN VICE", slots, stats, -1,
                         "[E] MOUNT WEAPON - FIT ATTACHMENTS", aspect);
    };
    m_panel.build(scene, device, hp);

    // Where the player stands to work: in FRONT of the bench.
    m_usePos = place(0.0f, 0.0f, 0.85f);

    m_built = true;
    x3::logInfo("[bench] weapon bench built (panel content: " +
                std::string(m_panel.screenHasContent() ? "BAKED" : "MISSING") + ")");
}

void WeaponBench::shutdown(x3::rhi::IRenderDevice& device) {
    m_panel.shutdown(device);
    for (auto m : m_meshes) if (m.valid()) device.destroyMesh(m);
    m_meshes.clear();
    m_ents.clear();
    m_built = false;
}

// ---------------------------------------------------------------------------
// Candidates: what can go in THIS slot on THIS weapon right now.
// index 0 is always "- EMPTY -" (i.e. remove whatever is fitted).
// ---------------------------------------------------------------------------
std::vector<std::string> WeaponBench::candidates(const Arsenal& ar, int weapon, AttachSlot s,
                                                 const Inventory& inv, const ItemDb& db) const {
    std::vector<std::string> out;
    out.push_back("");                                   // 0 = empty / remove
    if (weapon < 0 || weapon >= ar.count()) return out;
    const uint8_t mask = ar.baseDef(weapon).attachSlots;
    if ((mask & slotBit(s)) == 0) return out;            // this weapon refuses this slot
    for (int i = 0; i < Inventory::kBackpackSlots; ++i) {
        const InvSlot& sl = inv.slot(i);
        if (sl.empty()) continue;
        const ItemDef* d = db.find(sl.itemId);
        if (!d || !d->attach.valid || d->attach.slot != s) continue;
        if (std::find(out.begin(), out.end(), d->id) == out.end()) out.push_back(d->id);
    }
    return out;
}

void WeaponBench::openScreen(Arsenal& arsenal, const Inventory& inv, const ItemDb& db) {
    if (!m_built) return;
    m_open   = true;
    m_weapon = arsenal.selected() >= 0 ? arsenal.selected() : 0;
    m_row    = 0;
    for (int i = 0; i < kAttachSlotCount; ++i) m_cand[i] = 0;
    rebake(arsenal, inv, db);
}

void WeaponBench::closeScreen() {
    m_open = false;
}

void WeaponBench::tick(const Input& in, Arsenal& arsenal, Inventory& inv, const ItemDb& db,
                       const std::function<void()>& onChanged) {
    if (!m_built || !m_open) return;
    bool dirty = false, changed = false;

    if (in.up)   { m_row = (m_row + kAttachSlotCount) % (kAttachSlotCount + 1); dirty = true; }
    if (in.down) { m_row = (m_row + 1) % (kAttachSlotCount + 1); dirty = true; }

    if (m_row == 0) {
        // The WEAPON selector.
        if (in.left  && arsenal.count() > 0) {
            m_weapon = (m_weapon - 1 + arsenal.count()) % arsenal.count();
            for (int i = 0; i < kAttachSlotCount; ++i) m_cand[i] = 0;
            dirty = true;
        }
        if (in.right && arsenal.count() > 0) {
            m_weapon = (m_weapon + 1) % arsenal.count();
            for (int i = 0; i < kAttachSlotCount; ++i) m_cand[i] = 0;
            dirty = true;
        }
    } else {
        const AttachSlot s = (AttachSlot)(m_row - 1);
        const std::vector<std::string> cand = candidates(arsenal, m_weapon, s, inv, db);
        int& cur = m_cand[m_row - 1];
        if (cur >= (int)cand.size()) cur = 0;
        if (in.left)  { cur = (cur - 1 + (int)cand.size()) % (int)cand.size(); dirty = true; }
        if (in.right) { cur = (cur + 1) % (int)cand.size(); dirty = true; }

        if (in.activate) {
            const std::string want = cand[(size_t)cur];
            const AttachSpec* fitted = arsenal.loadouts().at(m_weapon).get(s);
            const std::string have = fitted ? fitted->id : std::string();
            if (want != have) {
                // Remove whatever is there first — it goes BACK IN THE BAG.
                if (!have.empty()) {
                    const std::string off = arsenal.unfitAttachment(m_weapon, s);
                    if (!off.empty())
                        if (const ItemDef* d = db.find(off)) inv.add(*d, 1);
                    changed = true;
                }
                // Fit the pick — it LEAVES the bag.
                if (!want.empty()) {
                    const ItemDef* d = db.find(want);
                    if (d && d->attach.valid) {
                        std::string displaced;
                        if (arsenal.fitAttachment(m_weapon, d->attach, &displaced)) {
                            // consume one from the backpack
                            for (int i = 0; i < Inventory::kBackpackSlots; ++i)
                                if (inv.slot(i).itemId == want && !inv.slot(i).empty()) {
                                    inv.removeAt(i, 1);
                                    break;
                                }
                            if (!displaced.empty())
                                if (const ItemDef* od = db.find(displaced)) inv.add(*od, 1);
                            changed = true;
                        }
                    }
                }
                dirty = true;
            }
        }
    }

    if (changed && onChanged) onChanged();
    if (dirty) rebake(arsenal, inv, db);
}

// ---------------------------------------------------------------------------
// The readout. The screen IS the UI.
// ---------------------------------------------------------------------------
void WeaponBench::rebake(Arsenal& arsenal, const Inventory& inv, const ItemDb& db) {
    if (!m_built) return;
    if (m_weapon < 0 || m_weapon >= arsenal.count()) m_weapon = 0;

    const WeaponDef& base = arsenal.baseDef(m_weapon);
    const WeaponLoadout& L = arsenal.loadouts().at(m_weapon);
    const WeaponDef eff = applyAttachments(base, L);   // THE fold — one place, reused here

    auto upper = [](std::string s) {
        for (char& ch : s) ch = (char)std::toupper((unsigned char)ch);
        return s;
    };

    std::vector<BenchRow> slots;
    for (int i = 0; i < kAttachSlotCount; ++i) {
        const AttachSlot s = (AttachSlot)i;
        BenchRow r;
        r.label = upper(attachSlotName(s));
        if ((base.attachSlots & slotBit(s)) == 0) {
            r.value = "NO MOUNT";      // this weapon does not accept this slot
            r.state = 2;
            slots.push_back(r);
            continue;
        }
        const std::vector<std::string> cand = candidates(arsenal, m_weapon, s, inv, db);
        int cur = m_cand[i];
        if (cur >= (int)cand.size()) cur = 0;
        const AttachSpec* fitted = L.get(s);
        const std::string sel = cand[(size_t)cur];
        if (m_row == i + 1 && sel != (fitted ? fitted->id : std::string())) {
            // A pending pick the player has cycled to but not committed.
            const ItemDef* d = sel.empty() ? nullptr : db.find(sel);
            r.value = "< " + std::string(d ? upper(d->name) : "REMOVE") + " ? >";
            r.state = 3;
        } else if (fitted) {
            r.value = upper(fitted->name);
            r.state = 1;
        } else {
            r.value = (cand.size() > 1) ? "- EMPTY -   (< >)" : "- EMPTY -";
            r.state = 0;
        }
        slots.push_back(r);
    }

    // Before -> after. GREEN when the number moved in your favour, ORANGE when it cost you.
    auto num = [](float v, int dec) {
        char buf[32];
        if (dec == 0) std::snprintf(buf, sizeof buf, "%d", (int)(v + 0.5f));
        else          std::snprintf(buf, sizeof buf, "%.2f", v);
        return std::string(buf);
    };
    auto statRow = [&](const char* label, float b, float e, bool lowerIsBetter, int dec) {
        BenchRow r;
        r.label = label;
        const bool same = std::fabs(e - b) < 1e-3f;
        r.value = same ? num(b, dec) : (num(b, dec) + " -> " + num(e, dec));
        if (same) r.state = 0;
        else      r.state = ((e < b) == lowerIsBetter) ? 1 : 2;
        return r;
    };
    std::vector<BenchRow> stats;
    stats.push_back(statRow("DMG",   (float)base.damage,  (float)eff.damage,  false, 0));
    stats.push_back(statRow("MAG",   (float)base.magSize, (float)eff.magSize, false, 0));
    stats.push_back(statRow("RLOAD", base.reloadTime,     eff.reloadTime,     true,  2));
    stats.push_back(statRow("SPRD",  base.spreadDeg,      eff.spreadDeg,      true,  2));
    stats.push_back(statRow("NOISE", base.noiseMult,      eff.noiseMult,      true,  2));
    stats.push_back(statRow("HANDL", base.handlingMult,   eff.handlingMult,   false, 2));

    const std::string hint = (m_row == 0)
        ? "UP/DOWN ROW - LEFT/RIGHT WEAPON - [E] CLOSE"
        : "LEFT/RIGHT PICK - ENTER FIT - [E] CLOSE";

    const float aspect = m_aspect;
    const std::string wname = upper(base.name);
    const int sel = m_row;
    m_panel.setContent(bakeBench(m_texN, wname, slots, stats, sel, hint, aspect));
}

} // namespace x3::game
