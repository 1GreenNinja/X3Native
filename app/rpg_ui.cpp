// RPG SCREENS (W9-3) — see rpg_ui.h.
#include "rpg_ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace x3::game {

using x3::ui::UiContext;
using FontRole = x3::rhi::FontRole;

// ---- palette ---------------------------------------------------------------
static const float kBackdrop[4]   = { 0.015f, 0.025f, 0.045f, 0.86f };
static const float kPanel[4]      = { 0.05f, 0.08f, 0.12f, 0.92f };
static const float kCell[4]       = { 0.09f, 0.13f, 0.18f, 0.95f };
static const float kCellSel[4]    = { 0.14f, 0.24f, 0.34f, 0.98f };
static const float kCellEdge[4]   = { 0.25f, 0.55f, 0.75f, 1.0f };
static const float kTitleCol[4]   = { 0.65f, 0.90f, 1.00f, 1.0f };
static const float kTextCol[4]    = { 0.85f, 0.90f, 0.95f, 1.0f };
static const float kDimCol[4]     = { 0.50f, 0.58f, 0.66f, 1.0f };
static const float kGoodCol[4]    = { 0.35f, 0.95f, 0.55f, 1.0f };
static const float kWarnCol[4]    = { 0.95f, 0.75f, 0.25f, 1.0f };
static const float kBadCol[4]     = { 0.95f, 0.35f, 0.30f, 1.0f };
static const float kOwnedFill[4]  = { 0.08f, 0.30f, 0.14f, 0.95f };
static const float kOwnedEdge[4]  = { 0.35f, 0.95f, 0.55f, 1.0f };
static const float kAvailFill[4]  = { 0.07f, 0.20f, 0.30f, 0.95f };
static const float kAvailEdge[4]  = { 0.25f, 0.80f, 1.00f, 1.0f };
static const float kLockFill[4]   = { 0.08f, 0.09f, 0.11f, 0.90f };
static const float kLockEdge[4]   = { 0.22f, 0.25f, 0.28f, 1.0f };
static const float kLine[4]       = { 0.30f, 0.42f, 0.52f, 0.9f };
static const float kLineOwned[4]  = { 0.30f, 0.75f, 0.45f, 0.9f };

static bool inRect(float mx, float my, float x, float y, float w, float h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// Greedy word wrap against the true proportional width.
static std::vector<std::string> wrapText(const std::string& s, float px, float maxW) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string cand = cur.empty() ? word : cur + " " + word;
        if (UiContext::textWidth(FontRole::Menu, cand.c_str(), px) <= maxW || cur.empty())
            cur = std::move(cand);
        else { lines.push_back(cur); cur = word; }
        word.clear();
    };
    for (char c : s) {
        if (c == ' ' || c == '\n') { flushWord(); if (c == '\n') { lines.push_back(cur); cur.clear(); } }
        else word += c;
    }
    flushWord();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// A bordered box: 1px edge + fill.
static void box(UiContext& ui, float x, float y, float w, float h,
                const float fill[4], const float edge[4]) {
    ui.quad(x - 1, y - 1, w + 2, h + 2, edge);
    ui.quad(x, y, w, h, fill);
}

void RpgUi::drawScreenBackdrop(UiContext& ui, const char* title, const char* hint) {
    ui.quad(0, 0, (float)ui.screenW(), (float)ui.screenH(), kBackdrop);
    ui.text(title, 60.0f, 34.0f, 34.0f, kTitleCol, FontRole::Title);
    const float hw = UiContext::textWidth(FontRole::Menu, hint, 15.0f);
    ui.text(hint, (float)ui.screenW() - hw - 60.0f, 46.0f, 15.0f, kDimCol);
}

// ===========================================================================
// BACKPACK
// ===========================================================================
void RpgUi::drawBackpack(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                         const Input& in, Inventory& inv, const ItemDb& db,
                         const UseItemFn& useItem) {
    UiContext ui;
    ui.begin(device, frame, in.ui);
    if (ui.screenW() == 0) { ui.end(); return; }
    m_pulse += 1.0f / 60.0f;

    drawScreenBackdrop(ui, "BACKPACK",
                       "ARROWS select   ENTER use   X drop   Q equip   I close");

    constexpr int kCols = 5, kRows = 4;
    const float cell = 68.0f, gap = 10.0f;
    const float gx = 60.0f, gy = 108.0f;

    // ---- selection: keyboard nav over backpack grid + key row --------------
    const int nBp = Inventory::kBackpackSlots;
    const int nKey = Inventory::kKeySlots;
    auto clampSel = [&](int s) { return s < 0 ? 0 : (s >= nBp + nKey ? nBp + nKey - 1 : s); };
    if (in.navLeft)  m_invSel = clampSel(m_invSel - 1);
    if (in.navRight) m_invSel = clampSel(m_invSel + 1);
    if (in.navUp) {
        if (m_invSel >= nBp) m_invSel = nBp - kCols + std::min(m_invSel - nBp, kCols - 1);
        else if (m_invSel >= kCols) m_invSel -= kCols;
    }
    if (in.navDown) {
        if (m_invSel < nBp - kCols) m_invSel += kCols;
        else if (m_invSel < nBp) m_invSel = nBp + std::min(m_invSel % kCols, nKey - 1);
    }

    const float keyY = gy + kRows * (cell + gap) + 34.0f;

    // ---- draw one slot cell (sz = cell size; the key row uses smaller cells) ----
    auto drawCell = [&](int selIdx, const InvSlot& s, float x, float y, float sz) {
        const bool sel = (m_invSel == selIdx);
        if (inRect(in.ui.mouseX, in.ui.mouseY, x, y, sz, sz)) {
            m_invSel = selIdx;
        }
        box(ui, x, y, sz, sz, sel ? kCellSel : kCell, sel ? kCellEdge : kLockEdge);
        if (!s.empty()) {
            const ItemDef* d = db.find(s.itemId);
            const char glyph[2] = { d ? d->glyph : '?', 0 };
            const float* gc = d ? d->color : kTextCol;
            const float gpx = sz * 0.44f;
            ui.textCentered(glyph, x + sz * 0.5f, y + sz * 0.2f, gpx, gc, FontRole::Title);
            if (s.count > 1) {
                char cnt[8]; std::snprintf(cnt, sizeof(cnt), "x%d", s.count);
                ui.text(cnt, x + sz - 26.0f, y + sz - 18.0f, 13.0f, kTextCol, FontRole::HudMono);
            }
            if (selIdx < nBp && inv.quickSlot() == selIdx)
                ui.text("Q", x + 4.0f, y + 3.0f, 13.0f, kWarnCol, FontRole::HudMono);
        }
    };

    // Backpack grid.
    for (int i = 0; i < nBp; ++i) {
        const float x = gx + (float)(i % kCols) * (cell + gap);
        const float y = gy + (float)(i / kCols) * (cell + gap);
        drawCell(i, inv.slot(i), x, y, cell);
    }
    char used[48];
    std::snprintf(used, sizeof(used), "SLOTS %d / %d", inv.usedSlots(), nBp);
    ui.text(used, gx, gy - 22.0f, 15.0f, kDimCol);

    // Key section row.
    ui.text("KEYCARDS + QUEST", gx, keyY - 22.0f, 15.0f, kDimCol);
    // Smaller cells + tighter pitch so all 8 key cells stay LEFT of the detail panel.
    for (int i = 0; i < nKey; ++i)
        drawCell(nBp + i, inv.keySlot(i), gx + (float)i * 50.0f, keyY, 44.0f);

    // ---- right: selected-item detail panel ---------------------------------
    const float px = gx + kCols * (cell + gap) + 26.0f;
    const float pw = std::min(430.0f, (float)ui.screenW() - px - 50.0f);
    const float ph = keyY + cell - gy;
    ui.panel(px, gy, pw, ph, kPanel);

    const bool selIsKey = m_invSel >= nBp;
    const InvSlot& sSel = selIsKey ? inv.keySlot(m_invSel - nBp) : inv.slot(m_invSel);
    const ItemDef* dSel = sSel.empty() ? nullptr : db.find(sSel.itemId);

    float ty = gy + 16.0f;
    if (!dSel) {
        ui.text(sSel.empty() ? "EMPTY SLOT" : "UNKNOWN ITEM", px + 18.0f, ty, 20.0f, kDimCol);
    } else {
        ui.text(dSel->name.c_str(), px + 18.0f, ty, 22.0f, dSel->color, FontRole::Menu);
        ty += 30.0f;
        char meta[64];
        std::snprintf(meta, sizeof(meta), "%s   x%d", itemCategoryName(dSel->cat), sSel.count);
        ui.text(meta, px + 18.0f, ty, 14.0f, kDimCol, FontRole::HudMono);
        ty += 26.0f;
        for (const std::string& ln : wrapText(dSel->desc, 15.0f, pw - 36.0f)) {
            ui.text(ln.c_str(), px + 18.0f, ty, 15.0f, kTextCol);
            ty += 20.0f;
        }
        // Effect readout.
        ty += 6.0f;
        auto fxLine = [&](const char* s) { ui.text(s, px + 18.0f, ty, 14.0f, kGoodCol, FontRole::HudMono); ty += 18.0f; };
        char fb[64];
        if (dSel->fx.heal)  { std::snprintf(fb, sizeof(fb), "+%d HP", dSel->fx.heal); fxLine(fb); }
        if (dSel->fx.ammo)  { std::snprintf(fb, sizeof(fb), "+%d RESERVE AMMO", dSel->fx.ammo); fxLine(fb); }
        if (dSel->fx.damageMult > 0)  { std::snprintf(fb, sizeof(fb), "+%d%% DAMAGE", (int)(dSel->fx.damageMult * 100 + 0.5f)); fxLine(fb); }
        if (dSel->fx.reloadMult > 0)  { std::snprintf(fb, sizeof(fb), "%d%% FASTER RELOAD", (int)(dSel->fx.reloadMult * 100 + 0.5f)); fxLine(fb); }
        if (dSel->fx.ammoCapMult > 0) { std::snprintf(fb, sizeof(fb), "+%d%% AMMO CAP", (int)(dSel->fx.ammoCapMult * 100 + 0.5f)); fxLine(fb); }
        if (dSel->fx.critChance > 0)  { std::snprintf(fb, sizeof(fb), "+%d%% CRIT", (int)(dSel->fx.critChance * 100 + 0.5f)); fxLine(fb); }
        if (dSel->fx.keycard > 0)     { fxLine("OPENS MATCHING LOCKED DOORS"); }

        // ---- verbs ----------------------------------------------------------
        const bool usable  = !selIsKey && (dSel->cat == ItemCategory::Consumable ||
                                           dSel->cat == ItemCategory::Mod);
        const bool dropable = !selIsKey;
        const float by = gy + ph - 52.0f;
        const char* useLabel = (dSel->cat == ItemCategory::Mod) ? "APPLY MOD" : "USE";
        bool doUse = false, doDrop = false, doEquip = false;
        if (usable   && ui.button(useLabel, px + 18.0f, by, 118.0f, 36.0f)) doUse = true;
        if (dropable && ui.button("DROP", px + 148.0f, by, 90.0f, 36.0f))   doDrop = true;
        if (usable && dSel->cat == ItemCategory::Consumable &&
            ui.button(inv.quickSlot() == m_invSel ? "UNEQUIP" : "EQUIP",
                      px + 250.0f, by, 104.0f, 36.0f)) doEquip = true;
        if (in.activate && usable) doUse = true;
        if (in.dropKey && dropable) doDrop = true;
        if (in.equipKey && usable && dSel->cat == ItemCategory::Consumable) doEquip = true;

        if (doUse && useItem) {
            if (useItem(*dSel)) inv.removeAt(m_invSel, 1);
        }
        if (doDrop) inv.removeAt(m_invSel, 1);
        if (doEquip) inv.setQuickSlot(inv.quickSlot() == m_invSel ? -1 : m_invSel);
    }

    ui.end();
}

// ===========================================================================
// SKILL TREE
// ===========================================================================
void RpgUi::drawSkills(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       const Input& in, SkillTree& tree, Progression& prog,
                       const StatsChangedFn& onChanged) {
    UiContext ui;
    ui.begin(device, frame, in.ui);
    if (ui.screenW() == 0 || tree.count() == 0) { ui.end(); return; }
    m_pulse += 1.0f / 60.0f;

    drawScreenBackdrop(ui, "SKILLS",
                       "ARROWS select   ENTER buy   K close");

    // Header: level + points.
    char hdr[96];
    std::snprintf(hdr, sizeof(hdr), "LEVEL %d    SKILL POINTS: %d", prog.level(), prog.skillPoints());
    ui.text(hdr, 60.0f, 76.0f, 18.0f, prog.skillPoints() > 0 ? kWarnCol : kDimCol, FontRole::HudMono);

    // ---- layout: branch columns, tier rows, sub-stacked same-tier nodes ----
    const auto& branches = tree.branches();
    const int nBranch = (int)branches.size();
    const float colW = std::min(300.0f, ((float)ui.screenW() - 120.0f) / (float)std::max(nBranch, 1));
    const float bw = colW - 56.0f, bh = 46.0f;
    const float topY = 140.0f;
    const float tierH = 128.0f, subH = 56.0f;

    struct NodePos { float x, y; int idx; };
    std::vector<NodePos> pos;
    pos.reserve(tree.count());
    for (int b = 0; b < nBranch; ++b) {
        // Branch header.
        const float cx = 60.0f + (float)b * colW;
        std::string bn = branches[(size_t)b];
        for (char& c : bn) c = (char)toupper((unsigned char)c);
        ui.text(bn.c_str(), cx, topY - 28.0f, 17.0f, kTitleCol, FontRole::Menu);
        // Nodes of this branch by tier, stacking same-tier vertically.
        int lastTier = -1, sub = 0;
        for (uint32_t i = 0; i < tree.count(); ++i) {
            const SkillNode& n = tree.at(i);
            if (n.branch != branches[(size_t)b]) continue;
            if (n.tier == lastTier) ++sub; else { sub = 0; lastTier = n.tier; }
            pos.push_back(NodePos{ cx, topY + (float)n.tier * tierH + (float)sub * subH, (int)i });
        }
    }
    auto posOf = [&](int nodeIdx) -> const NodePos* {
        for (const NodePos& p : pos) if (p.idx == nodeIdx) return &p;
        return nullptr;
    };
    auto idxOf = [&](std::string_view id) -> int {
        for (uint32_t i = 0; i < tree.count(); ++i) if (tree.at(i).id == id) return (int)i;
        return -1;
    };

    // ---- keyboard nav: up/down within branch, left/right across branches ----
    if (m_skillSel < 0 || m_skillSel >= (int)tree.count()) m_skillSel = 0;
    auto moveSel = [&](int dBranch, int dRow) {
        const SkillNode& cur = tree.at((uint32_t)m_skillSel);
        int curB = 0;
        for (int b = 0; b < nBranch; ++b) if (branches[(size_t)b] == cur.branch) curB = b;
        const NodePos* cp = posOf(m_skillSel);
        if (!cp) return;
        const int wantB = std::clamp(curB + dBranch, 0, nBranch - 1);
        // Choose the node in wantB whose y is nearest (cur.y + dRow*bias).
        const float wantY = cp->y + (float)dRow * (subH + 4.0f) + (dRow != 0 ? (float)dRow * 24.0f : 0.0f);
        int best = -1; float bestD = 1e9f;
        for (const NodePos& p : pos) {
            if (tree.at((uint32_t)p.idx).branch != branches[(size_t)wantB]) continue;
            if (dRow > 0 && p.y <= cp->y + 1.0f) continue;
            if (dRow < 0 && p.y >= cp->y - 1.0f) continue;
            const float d = std::fabs(p.y - wantY);
            if (d < bestD) { bestD = d; best = p.idx; }
        }
        if (best < 0 && dBranch != 0) {   // branch move with no row constraint: nearest y
            for (const NodePos& p : pos) {
                if (tree.at((uint32_t)p.idx).branch != branches[(size_t)wantB]) continue;
                const float d = std::fabs(p.y - cp->y);
                if (d < bestD) { bestD = d; best = p.idx; }
            }
        }
        if (best >= 0) m_skillSel = best;
    };
    if (in.navLeft)  moveSel(-1, 0);
    if (in.navRight) moveSel(+1, 0);
    if (in.navUp)    moveSel(0, -1);
    if (in.navDown)  moveSel(0, +1);

    // ---- prereq connector lines (under the boxes) ---------------------------
    for (const NodePos& p : pos) {
        const SkillNode& n = tree.at((uint32_t)p.idx);
        for (const std::string& pre : n.prereqs) {
            const int pi = idxOf(pre);
            const NodePos* pp = pi >= 0 ? posOf(pi) : nullptr;
            if (!pp) continue;
            const float* lc = tree.owned(pre) ? kLineOwned : kLine;
            const float x0 = pp->x + bw * 0.5f, y0 = pp->y + bh;
            const float x1 = p.x + bw * 0.5f,  y1 = p.y;
            const float midY = y0 + (y1 - y0) * 0.5f;
            ui.quad(x0 - 1.5f, y0, 3.0f, midY - y0, lc);
            const float lx = std::min(x0, x1), lw = std::fabs(x1 - x0);
            if (lw > 1.0f) ui.quad(lx, midY - 1.5f, lw, 3.0f, lc);
            ui.quad(x1 - 1.5f, midY, 3.0f, y1 - midY, lc);
        }
    }

    // ---- node boxes ---------------------------------------------------------
    for (const NodePos& p : pos) {
        const SkillNode& n = tree.at((uint32_t)p.idx);
        const bool isOwned = tree.owned(n.id);
        const bool avail   = tree.unlocked(n.id);
        const bool canBuy  = tree.canBuy(n.id, prog);
        const bool sel     = (m_skillSel == p.idx);
        if (inRect(in.ui.mouseX, in.ui.mouseY, p.x, p.y, bw, bh)) m_skillSel = p.idx;

        const float* fill = isOwned ? kOwnedFill : (avail ? kAvailFill : kLockFill);
        const float* edge = isOwned ? kOwnedEdge : (avail ? kAvailEdge : kLockEdge);
        float pulseEdge[4];
        if (canBuy && !isOwned) {   // affordable: pulse the edge
            const float s = 0.7f + 0.3f * std::sin(m_pulse * 4.0f);
            pulseEdge[0] = kAvailEdge[0] * s; pulseEdge[1] = kAvailEdge[1] * s;
            pulseEdge[2] = kAvailEdge[2] * s; pulseEdge[3] = 1.0f;
            edge = pulseEdge;
        }
        if (sel) { ui.quad(p.x - 4, p.y - 4, bw + 8, bh + 8, kCellEdge); }
        box(ui, p.x, p.y, bw, bh, fill, edge);
        const float* nameCol = isOwned ? kGoodCol : (avail ? kTextCol : kDimCol);
        ui.text(n.name.c_str(), p.x + 10.0f, p.y + 6.0f, 16.0f, nameCol);
        char costLn[32];
        if (isOwned) std::snprintf(costLn, sizeof(costLn), "OWNED");
        else         std::snprintf(costLn, sizeof(costLn), "%d PT%s", n.cost, n.cost > 1 ? "S" : "");
        ui.text(costLn, p.x + 10.0f, p.y + 26.0f, 12.0f,
                isOwned ? kGoodCol : (canBuy ? kWarnCol : kDimCol), FontRole::HudMono);
    }

    // ---- bottom: selected node detail + BUY + live stat preview -------------
    const float dpY = (float)ui.screenH() - 130.0f;
    ui.panel(60.0f, dpY, (float)ui.screenW() - 480.0f, 110.0f, kPanel);
    const SkillNode& selN = tree.at((uint32_t)m_skillSel);
    ui.text(selN.name.c_str(), 78.0f, dpY + 12.0f, 20.0f, kTitleCol);
    float dy = dpY + 40.0f;
    for (const std::string& ln : wrapText(selN.desc, 15.0f, (float)ui.screenW() - 700.0f)) {
        ui.text(ln.c_str(), 78.0f, dy, 15.0f, kTextCol);
        dy += 19.0f;
    }
    if (!selN.prereqs.empty() && !tree.unlocked(selN.id) && !tree.owned(selN.id)) {
        std::string req = "REQUIRES: ";
        for (size_t i = 0; i < selN.prereqs.size(); ++i) {
            const SkillNode* pn = tree.find(selN.prereqs[i]);
            req += pn ? pn->name : selN.prereqs[i];
            if (i + 1 < selN.prereqs.size()) req += ", ";
        }
        ui.text(req.c_str(), 78.0f, dy, 13.0f, kBadCol, FontRole::HudMono);
    }
    bool doBuy = false;
    if (tree.owned(selN.id)) {
        ui.text("OWNED", (float)ui.screenW() - 560.0f, dpY + 38.0f, 18.0f, kGoodCol);
    } else if (ui.button(tree.canBuy(selN.id, prog) ? "BUY" : "LOCKED",
                         (float)ui.screenW() - 590.0f, dpY + 32.0f, 120.0f, 42.0f)) {
        doBuy = true;
    }
    if (in.activate) doBuy = true;
    if (doBuy && tree.buy(selN.id, prog)) {
        if (onChanged) onChanged();
    }

    // Live stat preview (right column).
    {
        const float spX = (float)ui.screenW() - 400.0f, spW = 340.0f;
        ui.panel(spX, dpY, spW, 110.0f, kPanel);
        ui.text("LIVE STATS", spX + 16.0f, dpY + 10.0f, 14.0f, kDimCol, FontRole::HudMono);
        const PlayerStatMods m = tree.mods();
        // Short mono lines sized to the 340 px panel (max ~26 cells at 12 px).
        char l1[64], l2[64], l3[64], l4[64];
        std::snprintf(l1, sizeof(l1), "DMG x%.2f   CRIT %d%%",
                      m.damageMult, (int)(m.critChance * 100 + 0.5f));
        std::snprintf(l2, sizeof(l2), "HP +%d   SPD x%.2f", m.maxHpBonus, m.speedMult);
        std::snprintf(l3, sizeof(l3), "RELOAD x%.2f  AMMO x%.2f", m.reloadMult, m.ammoCapMult);
        std::snprintf(l4, sizeof(l4), "XP x%.2f  OWN %d  SPENT %d",
                      m.xpMult, tree.ownedCount(), prog.spentPoints());
        ui.text(l1, spX + 16.0f, dpY + 32.0f, 12.0f, kGoodCol, FontRole::HudMono);
        ui.text(l2, spX + 16.0f, dpY + 50.0f, 12.0f, kGoodCol, FontRole::HudMono);
        ui.text(l3, spX + 16.0f, dpY + 68.0f, 12.0f, kGoodCol, FontRole::HudMono);
        ui.text(l4, spX + 16.0f, dpY + 88.0f, 12.0f, kDimCol,  FontRole::HudMono);
    }

    ui.end();
}

// ===========================================================================
// HUD CHIP (equipped item + level/XP) + level-up toast
// ===========================================================================
void RpgUi::drawHudChip(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Inventory& inv, const ItemDb& db, const Progression& prog,
                        float dt) {
    UiContext ui;
    ui.begin(device, frame, x3::ui::UiInput{});
    if (ui.screenW() == 0) { ui.end(); return; }
    const float w = (float)ui.screenW(), h = (float)ui.screenH();

    // Level + XP sliver, bottom center-left of the chip row.
    const float chipY = h - 58.0f;
    const float lvX = w * 0.5f - 220.0f;
    {
        ui.panel(lvX, chipY, 150.0f, 44.0f, kPanel);
        char lv[24];
        std::snprintf(lv, sizeof(lv), "LV %d", prog.level());
        ui.text(lv, lvX + 12.0f, chipY + 6.0f, 17.0f, kTitleCol, FontRole::HudMono);
        const float frac = prog.xpForLevel() > 0
            ? (float)prog.xpIntoLevel() / (float)prog.xpForLevel() : 0.0f;
        const float fill[4] = { 0.35f, 0.80f, 1.00f, 1.0f };
        ui.bar(lvX + 12.0f, chipY + 30.0f, 126.0f, 7.0f, frac, fill);
        if (prog.skillPoints() > 0) {
            char pts[24];
            std::snprintf(pts, sizeof(pts), "+%d PT [K]", prog.skillPoints());
            ui.text(pts, lvX + 66.0f, chipY + 8.0f, 13.0f, kWarnCol, FontRole::HudMono);
        }
    }

    // Equipped quick item chip.
    const int q = inv.quickSlot();
    const InvSlot* qs = (q >= 0) ? &inv.slot(q) : nullptr;
    const ItemDef* qd = (qs && !qs->empty()) ? db.find(qs->itemId) : nullptr;
    {
        const float cx = w * 0.5f - 56.0f;
        ui.panel(cx, chipY, 210.0f, 44.0f, kPanel);
        if (qd) {
            const char glyph[2] = { qd->glyph, 0 };
            ui.text(glyph, cx + 10.0f, chipY + 8.0f, 24.0f, qd->color, FontRole::Title);
            ui.text(qd->name.c_str(), cx + 42.0f, chipY + 6.0f, 14.0f, kTextCol);
            char sub[32];
            std::snprintf(sub, sizeof(sub), "x%d   [Q] USE", qs->count);
            ui.text(sub, cx + 42.0f, chipY + 24.0f, 12.0f, kDimCol, FontRole::HudMono);
        } else {
            ui.text("NO ITEM EQUIPPED", cx + 12.0f, chipY + 8.0f, 13.0f, kDimCol, FontRole::HudMono);
            ui.text("[I] BACKPACK", cx + 12.0f, chipY + 25.0f, 12.0f, kDimCol, FontRole::HudMono);
        }
    }

    // Level-up toast (center, fading).
    if (m_toastTimer > 0.0f) {
        m_toastTimer -= dt;
        const float a = std::min(1.0f, m_toastTimer / 1.0f);
        float tc[4] = { kWarnCol[0], kWarnCol[1], kWarnCol[2], a };
        float sc[4] = { 0.0f, 0.0f, 0.0f, 0.75f * a };
        char t1[48], t2[64];
        std::snprintf(t1, sizeof(t1), "LEVEL UP  -  LEVEL %d", m_toastLevel);
        std::snprintf(t2, sizeof(t2), "+%d SKILL POINT  -  PRESS K TO SPEND", kPointsPerLevel);
        ui.textCentered(t1, w * 0.5f + 2.0f, h * 0.26f + 2.0f, 30.0f, sc, FontRole::Title);
        ui.textCentered(t1, w * 0.5f, h * 0.26f, 30.0f, tc, FontRole::Title);
        float dc[4] = { kTextCol[0], kTextCol[1], kTextCol[2], a };
        ui.textCentered(t2, w * 0.5f, h * 0.26f + 42.0f, 15.0f, dc);
    }

    ui.end();
}

} // namespace x3::game
