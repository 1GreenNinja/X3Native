// ============================================================================
// THE SHIP COMMS DEVICE — implementation. See ship_comms.h for the contract.
// ============================================================================
#include "ship_comms.h"

#include "ui.h"
#include "hud_panel.h"
#include "headless_device.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Sender identity. ONE table so the feed row, the filter tab and the focus rim
// can never disagree about what colour HOSTILE is.
//
// The inks are the game's established status family from hud_panel.h — cyan for
// the friendly system voice (the same ink the HUD uses for "your stuff"), red for
// the attacker, amber for the device talking about itself. Nothing new invented.
// ---------------------------------------------------------------------------
namespace {
constexpr float kInkShipAI[4]  = { 0.32f, 0.86f, 1.00f, 1.00f };   // kHudAccentCyan
constexpr float kInkHostile[4] = { 1.00f, 0.32f, 0.26f, 1.00f };   // hot red
constexpr float kInkSystem[4]  = { 1.00f, 0.72f, 0.20f, 1.00f };   // kHudAccentAmber
} // namespace

const char* commsSenderName(CommsSender s) {
    switch (s) {
        case CommsSender::ShipAI:  return "SHIP AI";
        case CommsSender::Hostile: return "HOSTILE";
        default:                   return "SYSTEM";
    }
}

const float* commsSenderInk(CommsSender s) {
    switch (s) {
        case CommsSender::ShipAI:  return kInkShipAI;
        case CommsSender::Hostile: return kInkHostile;
        default:                   return kInkSystem;
    }
}

// The iconography column. Deliberately ASCII: the HUD font atlas is the game's
// own, and a glyph outside it renders as a hole. These read as a transponder
// diamond (friendly), a warning bang (hostile) and a device dot (system).
const char* commsSenderGlyph(CommsSender s) {
    switch (s) {
        case CommsSender::ShipAI:  return "<>";
        case CommsSender::Hostile: return "!!";
        default:                   return "::";
    }
}

// ===========================================================================
// CommsDevice — store
// ===========================================================================
uint32_t CommsDevice::post(CommsSender sender, const char* from, const char* text) {
    CommsMessage m;
    m.sender = sender;
    m.from   = from ? from : "";
    m.text   = text ? text : "";
    m.beat   = ++m_beat;
    m.stamp  = m_clock;
    m.age    = 0.0f;
    m.acked  = false;
    m_log.push_back(std::move(m));

    // THE BACKLOG CAP. Drop from the FRONT so the newest lines always survive —
    // a comms feed that discarded the message that just arrived would be worse
    // than useless during a fight.
    while ((int)m_log.size() > kCommsBacklogCap) m_log.pop_front();

    m_arrival = 1.0f;      // kick the arrival glow
    m_scroll  = 0;         // a new line snaps the view back to the newest
    return m_log.back().beat;
}

void CommsDevice::clear() {
    m_log.clear();
    m_scroll  = 0;
    m_arrival = 0.0f;
}

int CommsDevice::unacked() const {
    int n = 0;
    for (const auto& m : m_log) if (!m.acked) ++n;
    return n;
}

// ===========================================================================
// CommsDevice — focus
// ===========================================================================
void CommsDevice::setFocused(bool on) {
    if (m_focused == on) return;
    m_focused = on;
    // Leaving focus drops any scrollback so the next glance is at live traffic,
    // and re-arms the arrival glow bookkeeping. It deliberately does NOT clear
    // the filter: a player who tuned to HOSTILE meant it.
    if (!on) m_scroll = 0;
}

void CommsDevice::setFilter(int f) {
    m_filter = (f < 0) ? 0 : (f > 2 ? 2 : f);
    m_tab    = m_filter;
    m_scroll = 0;
}

void CommsDevice::scroll(int lines) {
    m_scroll += lines;
    if (m_scroll < 0) m_scroll = 0;
    // The upper clamp needs the wrapped-line count, which only draw() knows.
    // draw() re-clamps every frame; this keeps the headless path sane.
    const int cap = (int)m_log.size() * 4;
    if (m_scroll > cap) m_scroll = cap;
}

void CommsDevice::ackAll() {
    for (auto& m : m_log) m.acked = true;
}

// HAIL is a STUB WITH A STATED CONTRACT. It does not open a channel today: the
// hostile side has no reply model and the ship AI's lines are authored, so a
// "reply" would have to invent content this lane is not authorised to write.
// What it DOES do is real and observable — it posts an outbound line to the feed
// and increments hailCount(), which is the seam a future reply system (or the
// dialog.h LLM provider) hangs off. It is deliberately not a no-op button.
void CommsDevice::hail() {
    ++m_hailCount;
    post(CommsSender::System, "COMMS", "HAIL SENT - NO CARRIER. CHANNEL LOGGED.");
}

// ===========================================================================
// CommsDevice — per-frame
// ===========================================================================
void CommsDevice::update(float dt) {
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;          // hitch clamp, same as the shell's
    m_clock += dt;
    for (auto& m : m_log) m.age += dt;
    // dt-CORRECT DECAY, not a per-frame subtract: the arrival glow must last the
    // same ~1.2 s at 60 Hz and at 165 Hz. (House rule, learned the hard way on a
    // per-frame zoom that "WHAM"d at 165 Hz.)
    if (m_arrival > 0.0f) {
        m_arrival -= dt / 1.2f;
        if (m_arrival < 0.0f) m_arrival = 0.0f;
    }
}

bool CommsDevice::passesFilter(const CommsMessage& m) const {
    if (m_filter == 0) return true;
    if (m_filter == 1) return m.sender == CommsSender::ShipAI || m.sender == CommsSender::System;
    return m.sender == CommsSender::Hostile;
}

namespace {

// Wrap `text` to `maxW` pixels at glyph size `px` in `role`. Greedy word wrap
// with a hard character-split fallback so a single long token cannot overflow the
// plate (call signs and world keys have no spaces).
void wrapInto(std::vector<std::string>& out, const std::string& text,
              float maxW, float px, x3::rhi::FontRole role) {
    if (text.empty()) { out.emplace_back(); return; }
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t sp = text.find(' ', i);
        if (sp == std::string::npos) sp = text.size();
        std::string word = text.substr(i, sp - i);
        std::string cand = line.empty() ? word : line + " " + word;
        if (x3::ui::UiContext::textWidth(role, cand.c_str(), px) <= maxW) {
            line = std::move(cand);
        } else if (line.empty()) {
            // One word wider than the plate: hard-split it.
            std::string chunk;
            for (char c : word) {
                std::string t = chunk + c;
                if (x3::ui::UiContext::textWidth(role, t.c_str(), px) > maxW && !chunk.empty()) {
                    out.push_back(chunk);
                    chunk.clear();
                }
                chunk += c;
            }
            line = chunk;
        } else {
            out.push_back(line);
            line = word;
        }
        i = sp + 1;
    }
    if (!line.empty()) out.push_back(line);
    if (out.empty()) out.emplace_back();
}

// MM:SS beat marker from the device clock.
void stampText(float stamp, char* buf, size_t cap) {
    const int total = (int)stamp;
    std::snprintf(buf, cap, "%02d:%02d", (total / 60) % 100, total % 60);
}

// One display row: which message it came from and the text to draw.
struct FeedLine {
    const CommsMessage* msg = nullptr;
    std::string         text;
    bool                head = false;   // first line of the message (draws the header)
};

} // namespace

void CommsDevice::draw(x3::ui::UiContext& ui, x3::rhi::IRenderDevice& device,
                       const x3::rhi::FrameContext& frame, float dt) {
    update(dt);

    const float W = (float)ui.screenW();
    const float H = (float)ui.screenH();
    if (W <= 0.0f || H <= 0.0f) { m_rect[2] = m_rect[3] = 0.0f; return; }

    // ---- GEOMETRY -----------------------------------------------------------
    // RIGHT-ANCHORED and COMPACT, per the spec. The plate's left edge sits at
    // ~0.72 W, which keeps it clear of the reticle (screen centre) by a wide
    // margin, and it is TOP-anchored so the lower-right stays free for the
    // hardpoint labels that bracket the capital's hull at engagement range.
    const float panelW = std::clamp(W * 0.235f, 280.0f, 420.0f);
    const float panelH = std::clamp(H * 0.42f,  230.0f, 470.0f);
    const float px0 = W - panelW - kHudMargin;
    const float py0 = kHudMargin * 2.0f;
    m_rect[0] = px0; m_rect[1] = py0; m_rect[2] = panelW; m_rect[3] = panelH;

    const HudPanelTuning& tune = hudPanelTuning();
    const float radius = tune.radius;

    // The accent bar takes the ink of the NEWEST sender, so the device's own edge
    // tells you who last spoke before you read a word of it.
    const CommsMessage* top = newest();
    const float* accent = top ? commsSenderInk(top->sender) : kInkSystem;

    // ---- THE GLASS ----------------------------------------------------------
    // Base plate at the MEASURED 0.86 alpha. Do not soften this: more
    // transparent composites to mid-grey over a bright scene and the light text
    // washes out (hud_panel.h documents the measurement).
    hudPanel(device, frame, px0, py0, panelW, panelH, radius,
             tune.fill, accent, 1.0f, /*topEdge*/true, true, true);

    // Glass character ON TOP of the shared plate — this is the "glassy,
    // futuristic, translucent" ask, built from the same primitives so it stays
    // inside the one visual system rather than inventing a second look.
    const float glow = m_arrival;          // 0..1, decays over 1.2 s

    // (a) SCANLINES — a faint horizontal rule every 3 px. Reads as an emissive
    //     panel rather than a painted rectangle, and is nearly free (thin quads
    //     through the same HUD path the rounded corners already use).
    {
        const float scan[4] = { 0.45f, 0.75f, 0.95f, 0.030f };
        for (float sy = py0 + 3.0f; sy < py0 + panelH - 2.0f; sy += 3.0f)
            ui.quad(px0 + 1.0f, sy, panelW - 2.0f, 1.0f, scan);
    }

    // (b) SHEEN — a soft band drifting down the plate on the free-running clock,
    //     the specular crawl of a curved glass face. dt-driven, so it moves at
    //     the same rate at 60 Hz and 165 Hz.
    {
        const float sweep = std::fmod(m_clock * 0.13f, 1.0f);
        const float band  = py0 + sweep * panelH;
        for (int i = 0; i < 7; ++i) {
            const float f = 1.0f - (float)i / 7.0f;
            const float a = 0.030f * f * f;
            const float yy = band + (float)i * 2.0f;
            if (yy > py0 && yy < py0 + panelH - 1.0f) {
                const float sh[4] = { 0.60f, 0.85f, 1.00f, a };
                ui.quad(px0 + 1.0f, yy, panelW - 2.0f, 2.0f, sh);
            }
        }
    }

    // (c) RIM — a 1 px edge highlight down both sides and along the bottom.
    //     hudPanel() already draws the top edge, so this closes the box and gives
    //     the plate a lit bevel instead of a cut-out silhouette. While FOCUSED
    //     the rim goes 2 px and takes the accent ink, pulsing gently: that is the
    //     unmissable "this panel owns your keyboard" state.
    {
        const bool  f      = m_focused;
        const float pulse  = f ? (0.55f + 0.45f * std::sin(m_clock * 4.0f)) : 0.0f;
        const float thick  = f ? 2.0f : 1.0f;
        float rim[4];
        if (f) {
            rim[0] = accent[0]; rim[1] = accent[1]; rim[2] = accent[2];
            rim[3] = 0.35f + 0.45f * pulse;
        } else {
            rim[0] = 0.45f; rim[1] = 0.75f; rim[2] = 0.95f;
            rim[3] = 0.16f + 0.34f * glow;      // arrival makes the edge flare
        }
        ui.quad(px0,                 py0, thick,  panelH, rim);           // left
        ui.quad(px0 + panelW - thick,py0, thick,  panelH, rim);           // right
        ui.quad(px0, py0 + panelH - thick, panelW, thick, rim);           // bottom
        if (f) ui.quad(px0, py0, panelW, thick, rim);                     // top (focus only)
    }

    // ---- HEADER -------------------------------------------------------------
    const float padX = kHudPadX;
    float cy = py0 + kHudPadY;

    {
        ui.text("COMMS", px0 + padX, cy, 13.0f, kHudTextLight, x3::rhi::FontRole::Menu);

        // Right-aligned state chip: the focus contract, spelled out on the glass.
        // A player must never have to guess whether this thing has his keyboard.
        const char* state = m_focused ? "FOCUS - ESC TO FLY" : "F10 FOCUS";
        float chipInk[4];
        if (m_focused) {
            chipInk[0] = accent[0]; chipInk[1] = accent[1];
            chipInk[2] = accent[2]; chipInk[3] = 1.0f;
        } else {
            chipInk[0] = 0.62f; chipInk[1] = 0.72f; chipInk[2] = 0.80f; chipInk[3] = 1.0f;
        }
        const float cw = x3::ui::UiContext::textWidth(x3::rhi::FontRole::News, state, 9.0f);
        ui.text(state, px0 + panelW - padX - cw, cy + 2.0f, 9.0f, chipInk,
                x3::rhi::FontRole::News);
        cy += hudLineH(13.0f);
    }

    // ---- FILTER TABS (UiContext::tabBar — the shared widget, not a hand-roll) --
    {
        static const char* const kTabs[3] = { "ALL", "AI", "HOSTILE" };
        ui.beginLayout(px0 + padX, cy, panelW - padX * 2.0f, 20.0f, 4.0f);
        m_tab = m_filter;
        if (ui.tabBar(kTabs, 3, m_tab, 20.0f)) setFilter(m_tab);
        cy = ui.cursorY() + 4.0f;
    }

    // ---- BUTTON ROW (reserved at the BOTTOM, laid out before the feed so the
    //      feed knows how much room it actually has) ---------------------------
    const float btnH   = 22.0f;
    const float btnTop = py0 + panelH - kHudPadY - btnH;

    // ---- THE FEED -----------------------------------------------------------
    // NEWEST AT THE BOTTOM. Justification, since the spec asked for one: this is
    // a comms LOG, and every log surface the player already uses in this game —
    // above all the in-game console, which is this device's sibling reference —
    // grows downward with the newest line nearest the input row. Matching it
    // means the eye lands in the same place in both surfaces, and the buttons sit
    // under the newest message where the hand already is. Newest-at-top would
    // read as a notification stack, which is what this is NOT: it has scrollback.
    const float feedTop = cy;
    const float feedBot = btnTop - 6.0f;
    const float feedH   = feedBot - feedTop;
    const float bodyPx  = 10.0f;
    const float lineH   = hudLineH(bodyPx);
    m_visRows = (feedH > 0.0f) ? (int)(feedH / lineH) : 0;

    {
        const float glyphW = 20.0f;
        const float textX  = px0 + padX + glyphW;
        const float textW  = panelW - padX * 2.0f - glyphW;

        // Flatten the filtered log into wrapped display lines, oldest first.
        std::vector<FeedLine> lines;
        lines.reserve(m_log.size() * 2);
        for (const auto& m : m_log) {
            if (!passesFilter(m)) continue;
            std::vector<std::string> wrapped;
            wrapInto(wrapped, m.text, textW, bodyPx, x3::rhi::FontRole::News);
            for (size_t k = 0; k < wrapped.size(); ++k) {
                FeedLine fl;
                fl.msg  = &m;
                fl.text = wrapped[k];
                fl.head = (k == 0);
                lines.push_back(std::move(fl));
            }
        }

        // Each message costs one extra row for its "SENDER  MM:SS" header.
        std::vector<FeedLine> disp;
        disp.reserve(lines.size() + m_log.size());
        const CommsMessage* prev = nullptr;
        for (auto& fl : lines) {
            if (fl.msg != prev) {
                FeedLine hdr;
                hdr.msg = fl.msg; hdr.head = true; hdr.text.clear();
                disp.push_back(std::move(hdr));
                prev = fl.msg;
            }
            FeedLine body = fl;
            body.head = false;
            disp.push_back(std::move(body));
        }

        const int total = (int)disp.size();
        // Re-clamp the scroll against the REAL wrapped-line count.
        const int maxScroll = std::max(0, total - m_visRows);
        if (m_scroll > maxScroll) m_scroll = maxScroll;
        const int end   = total - m_scroll;              // one past the last shown
        const int start = std::max(0, end - m_visRows);

        float ly = feedTop;
        for (int i = start; i < end && i < total; ++i) {
            const FeedLine& fl = disp[(size_t)i];
            if (!fl.msg) { ly += lineH; continue; }
            const float* ink = commsSenderInk(fl.msg->sender);

            if (fl.head) {
                // The attribution row: glyph + sender + callsign + beat marker.
                // UNACKED lines get the full-strength ink; acknowledged ones dim,
                // so "what is new" is legible without reading any words.
                const float a = fl.msg->acked ? 0.45f : 1.0f;
                const float hi[4] = { ink[0], ink[1], ink[2], a };
                ui.text(commsSenderGlyph(fl.msg->sender), px0 + padX, ly, bodyPx, hi,
                        x3::rhi::FontRole::News);
                std::string who = fl.msg->from.empty()
                                ? commsSenderName(fl.msg->sender)
                                : fl.msg->from;
                ui.text(who.c_str(), textX, ly, bodyPx, hi, x3::rhi::FontRole::News);

                char st[16];
                stampText(fl.msg->stamp, st, sizeof(st));
                const float sw = x3::ui::UiContext::textWidth(x3::rhi::FontRole::News, st, bodyPx);
                const float dim[4] = { 0.55f, 0.64f, 0.72f, a };
                ui.text(st, px0 + panelW - padX - sw, ly, bodyPx, dim, x3::rhi::FontRole::News);
            } else if (!fl.text.empty()) {
                const float a = fl.msg->acked ? 0.55f : 1.0f;
                // Hostile BODY text keeps a warm tint; the ship AI's body text is
                // the standard light HUD ink, so the attacker's lines read as
                // intrusions and the AI's as instrumentation.
                float body[4] = { kHudTextLight[0], kHudTextLight[1], kHudTextLight[2], a };
                if (fl.msg->sender == CommsSender::Hostile) {
                    body[0] = 1.00f; body[1] = 0.78f; body[2] = 0.74f;
                }
                ui.text(fl.text.c_str(), textX, ly, bodyPx, body, x3::rhi::FontRole::News);
            }
            ly += lineH;
        }

        // IDLE STATE — the "a host with nothing to say" surface. Never a blank
        // plate and never a crash: the device says, plainly, that the channel is
        // up and quiet.
        if (total == 0) {
            const float idle[4] = { 0.52f, 0.60f, 0.68f, 0.85f };
            ui.text("CHANNEL OPEN", px0 + padX, feedTop + 4.0f, bodyPx, idle,
                    x3::rhi::FontRole::News);
            ui.text("NO TRAFFIC", px0 + padX, feedTop + 4.0f + lineH, bodyPx, idle,
                    x3::rhi::FontRole::News);
        }

        // Scrollback indicator: only when there IS something above the fold.
        if (m_scroll > 0) {
            const float m[4] = { accent[0], accent[1], accent[2], 0.75f };
            ui.text("^ SCROLLBACK", px0 + padX, feedTop - lineH + 2.0f, 8.0f, m,
                    x3::rhi::FontRole::News);
        }
    }

    // ---- BUTTONS ------------------------------------------------------------
    // All four are UiContext::button — the shared widget, with the shared focus
    // ring and the shared hover highlight. No hand-rolled hit-testing anywhere.
    {
        const float gap = 5.0f;
        const float availW = panelW - padX * 2.0f;
        const float smallW = 26.0f;
        const float wideW  = (availW - smallW * 2.0f - gap * 3.0f) * 0.5f;
        float bx = px0 + padX;

        if (ui.button("ACK", bx, btnTop, wideW, btnH)) ackAll();
        bx += wideW + gap;
        if (ui.button("HAIL", bx, btnTop, wideW, btnH)) hail();
        bx += wideW + gap;
        if (ui.button("^", bx, btnTop, smallW, btnH)) scroll(+3);
        bx += smallW + gap;
        if (ui.button("v", bx, btnTop, smallW, btnH)) scroll(-3);
    }
}

// ===========================================================================
// The key router. See the contract in the header.
// ===========================================================================
bool commsRouteKey(CommsDevice& dev, CommsKey k) {
    // UNFOCUSED: the device is a display, not an input sink. It takes ONE key —
    // the one that focuses it — and lets everything else through to flight.
    if (!dev.focused()) {
        if (k == CommsKey::Focus) { dev.setFocused(true); return true; }
        return false;
    }

    switch (k) {
        case CommsKey::Focus:
        case CommsKey::Escape:     dev.setFocused(false);          return true;
        case CommsKey::ScrollUp:   dev.scroll(+3);                 return true;
        case CommsKey::ScrollDown: dev.scroll(-3);                 return true;
        case CommsKey::NextFilter: dev.setFilter((dev.filter() + 1) % 3); return true;
        case CommsKey::Ack:        dev.ackAll();                   return true;
        case CommsKey::Hail:       dev.hail();                     return true;
        case CommsKey::Other:
        case CommsKey::None:
        default:
            // Swallowed so a focused panel never leaks a keystroke into flight —
            // the same discipline the console and the tuning panel already use.
            return true;
    }
}

// ===========================================================================
// CommsDirector — the authored lines, keyed to real events
// ===========================================================================
void CommsDirector::reset() {
    *this = CommsDirector();
}

CommsDirector::PortalMemo& CommsDirector::memoFor(int id) {
    for (int i = 0; i < m_portalMemos; ++i)
        if (m_portals[i].id == id) return m_portals[i];
    if (m_portalMemos < 16) {
        m_portals[m_portalMemos].id = id;
        return m_portals[m_portalMemos++];
    }
    return m_portals[15];        // saturate rather than overflow
}

int CommsDirector::advisoriesFor(int portalId) const {
    for (int i = 0; i < m_portalMemos; ++i)
        if (m_portals[i].id == portalId) return m_portals[i].count;
    return 0;
}

void CommsDirector::update(CommsDevice& dev, const CommsSnapshot& snap, float dt) {
    (void)dt;

    // ---- WORMHOLE ADVISORIES — the ship AI's headline duty -------------------
    // Fires on the player CLOSING inside the advisory range, and re-arms only
    // after he leaves the wider hysteresis band. Without the two-radius band a
    // player hovering on the boundary at 165 Hz would machine-gun the feed.
    for (int i = 0; i < snap.portalCount && snap.portals; ++i) {
        const CommsPortal& p = snap.portals[i];
        const float dx = p.pos[0] - snap.playerPos[0];
        const float dy = p.pos[1] - snap.playerPos[1];
        const float dz = p.pos[2] - snap.playerPos[2];
        const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);

        PortalMemo& memo = memoFor(p.id);
        if (!memo.announced && d <= kCommsPortalAdvisoryRange) {
            memo.announced = true;
            ++memo.count;
            char line[192];
            // The advisory NAMES the wormhole and states its stability, which is
            // exactly the sentence Tim asked for.
            std::snprintf(line, sizeof(line),
                          "%s WORMHOLE %.0fm - %s. %s",
                          p.stable ? "STABLE" : "UNSTABLE",
                          d,
                          p.name && p.name[0] ? p.name : "UNCHARTED",
                          p.stable ? "Transit corridor is holding."
                                   : "Aperture is fluctuating - transit not advised.");
            dev.post(CommsSender::ShipAI, kCommsShipAiName, line);
        } else if (memo.announced && d > kCommsPortalRearmRange) {
            memo.announced = false;      // left the band: the next approach re-arms
        }
    }

    // Everything below is space-only. A world that never publishes inSpace gets
    // an idle device — that is the "host with no content" contract.
    if (!snap.inSpace) return;

    if (!m_greeted) {
        m_greeted = true;
        dev.post(CommsSender::ShipAI, kCommsShipAiName,
                 "Comms online. I have the channel, Commander.");
    }

    // ---- HOSTILE TRANSMISSIONS, keyed to REAL encounter events ---------------
    // Each of these fires on a rising edge and never again for that run, so a
    // taunt lands on the beat that earned it and the feed does not repeat itself.

    // FIRST BLOOD — the player has taken damage for the first time.
    if (snap.playerTookFirstHit && !m_seenFirstHit) {
        m_seenFirstHit = true;
        dev.post(CommsSender::Hostile, kCommsHostileName,
                 "First blood. You fly like something that has never been hunted.");
    }

    // A MOUNT SHEARED — one of the capital's hardpoints has come off.
    if (snap.mountsDestroyed > m_seenMounts) {
        const int sheared = snap.mountsDestroyed;
        m_seenMounts = sheared;
        char line[160];
        if (sheared == 1) {
            std::snprintf(line, sizeof(line),
                          "You sheared a mount. I have three more and all the time there is.");
        } else if (sheared >= 4) {
            std::snprintf(line, sizeof(line),
                          "Every mount gone. You have made this personal.");
        } else {
            std::snprintf(line, sizeof(line),
                          "Mount %d is scrap. Keep going. I want to see how close you get.",
                          sheared);
        }
        dev.post(CommsSender::Hostile, kCommsHostileName, line);
    }

    // BAYS LAUNCHING — the taunt that rides the encounter's own existing bark.
    if (snap.baysLaunching && !m_seenBays) {
        m_seenBays = true;
        dev.post(CommsSender::Hostile, kCommsHostileName,
                 "Launching. You will not out-fly the whole wing.");
        dev.post(CommsSender::ShipAI, kCommsShipAiName,
                 "Fighters away from her bays. Kill the bays or this does not end.");
    }

    // SHIELDS DOWN on the capital.
    if (snap.capitalShieldsDown && !m_seenShieldsDown) {
        m_seenShieldsDown = true;
        dev.post(CommsSender::Hostile, kCommsHostileName,
                 "Shields are down. It changes nothing.");
        dev.post(CommsSender::ShipAI, kCommsShipAiName,
                 "Her shields are down. Hull is exposed - press it.");
    }

    // REACTOR BREACH — the last thing she says.
    if (snap.reactorBreach && !m_seenBreach) {
        m_seenBreach = true;
        dev.post(CommsSender::Hostile, kCommsHostileName,
                 "Reactor breach. You have killed us both, pilot.");
        dev.post(CommsSender::ShipAI, kCommsShipAiName,
                 "Reactor breach confirmed. Get us clear of the blast envelope. NOW.");
    }

    // PHASE CHANGE — a neutral ship-AI note, not a taunt.
    if (snap.phase != m_seenPhase) {
        if (m_seenPhase >= 0) {
            char line[96];
            std::snprintf(line, sizeof(line), "Engagement phase %d.", snap.phase);
            dev.post(CommsSender::ShipAI, kCommsShipAiName, line);
        }
        m_seenPhase = snap.phase;
    }

    // ---- SHIP AI SYSTEMS CHATTER on real player-ship state -------------------
    // Threshold crossings with hysteresis in BOTH directions, so a shield sitting
    // exactly on 0.30 while it regenerates cannot chatter every frame.
    if (!m_lowShield && snap.shieldFrac <= 0.30f) {
        m_lowShield = true;
        dev.post(CommsSender::ShipAI, kCommsShipAiName,
                 "Shields at thirty percent. Break contact and let them cycle.");
    } else if (m_lowShield && snap.shieldFrac >= 0.55f) {
        m_lowShield = false;
        dev.post(CommsSender::ShipAI, kCommsShipAiName, "Shields recovered.");
    }

    if (!m_lowHull && snap.hullFrac <= 0.35f) {
        m_lowHull = true;
        dev.post(CommsSender::ShipAI, kCommsShipAiName,
                 "Hull integrity critical. I cannot patch this in the air.");
    } else if (m_lowHull && snap.hullFrac >= 0.60f) {
        m_lowHull = false;
    }

    if (!m_lowWeapon && snap.weaponFrac <= 0.15f) {
        m_lowWeapon = true;
        dev.post(CommsSender::ShipAI, kCommsShipAiName,
                 "Weapon energy dry. Ease off the trigger and let the bank refill.");
    } else if (m_lowWeapon && snap.weaponFrac >= 0.45f) {
        m_lowWeapon = false;
    }

    // INCOMING FIGHTERS — only on a RISE, and only when it crosses into a wing.
    if (snap.incomingFighters > m_lastFighters && snap.incomingFighters >= 3) {
        char line[96];
        std::snprintf(line, sizeof(line), "%d contacts inbound. Watch your six.",
                      snap.incomingFighters);
        dev.post(CommsSender::ShipAI, kCommsShipAiName, line);
    }
    m_lastFighters = snap.incomingFighters;
}

// ===========================================================================
// CommsBus
// ===========================================================================
CommsBus& commsBus() {
    static CommsBus bus;
    return bus;
}

void CommsBus::post(CommsSender sender, const char* from, const char* text) {
    Pending p;
    p.sender = sender;
    p.from   = from ? from : "";
    p.text   = text ? text : "";
    m_queue.push_back(std::move(p));
    while ((int)m_queue.size() > kCommsBusQueueCap) m_queue.pop_front();
}

void CommsBus::publishShip(bool inSpace, float shieldFrac, float hullFrac,
                           float weaponFrac, int incomingFighters) {
    m_snap.inSpace    = inSpace;
    m_snap.shieldFrac = shieldFrac;
    m_snap.hullFrac   = hullFrac;
    m_snap.weaponFrac = weaponFrac;
    m_snap.incomingFighters = incomingFighters;
}

void CommsBus::publishEncounter(int phase, bool firstHit, int mountsDestroyed,
                                bool baysLaunching, bool capitalShieldsDown,
                                bool reactorBreach) {
    m_snap.phase              = phase;
    m_snap.playerTookFirstHit = firstHit;
    m_snap.mountsDestroyed    = mountsDestroyed;
    m_snap.baysLaunching      = baysLaunching;
    m_snap.capitalShieldsDown = capitalShieldsDown;
    m_snap.reactorBreach      = reactorBreach;
}

void CommsBus::publishPortals(const CommsPortal* portals, int count, const float eye[3]) {
    if (eye) {
        m_snap.playerPos[0] = eye[0];
        m_snap.playerPos[1] = eye[1];
        m_snap.playerPos[2] = eye[2];
    }
    m_portalCount = 0;
    if (!portals || count <= 0) { m_snap.portals = nullptr; m_snap.portalCount = 0; return; }
    const int n = std::min(count, kCommsMaxPortals);
    for (int i = 0; i < n; ++i) {
        m_portals[i] = portals[i];
        // The publisher's name pointer may be a temporary; copy the text so the
        // snapshot stays valid until the drain.
        const char* src = portals[i].name ? portals[i].name : "";
        std::snprintf(m_portalNames[i], sizeof(m_portalNames[i]), "%s", src);
        m_portals[i].name = m_portalNames[i];
    }
    m_portalCount      = n;
    m_snap.portals     = m_portals;
    m_snap.portalCount = n;
}

void CommsBus::drain(CommsDevice& dev, CommsSnapshot& outSnap) {
    for (auto& p : m_queue) dev.post(p.sender, p.from.c_str(), p.text.c_str());
    m_queue.clear();
    outSnap = m_snap;
    // Re-point the snapshot at the bus's OWN storage (the copy above copied the
    // pointer, which is correct, but this makes the aliasing explicit).
    outSnap.portals     = m_portalCount ? m_portals : nullptr;
    outSnap.portalCount = m_portalCount;
}

void CommsBus::reset() {
    m_queue.clear();
    m_snap = CommsSnapshot();
    m_portalCount = 0;
}

// ===========================================================================
// --test-comms — headless self-test. No window, no Vulkan, no world.
// ===========================================================================
namespace {

// The UI tests need a non-zero hudSize so the panel's layout math actually runs;
// everything else is the shared no-op device. An INVALID FrameContext makes
// UiContext skip draws while still hit-testing, so the button paths below are the
// REAL ones the game runs, not a parallel mock.
class CommsStubDevice final : public x3::game::HeadlessRenderDevice {
public:
    void hudSize(uint32_t& w, uint32_t& h) const override { w = 1920; h = 1080; }
};

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* what) {
    if (cond) { ++g_pass; x3::logInfo(std::string("  [ok]   ") + what); }
    else      { ++g_fail; x3::logError(std::string("  [FAIL] ") + what); }
}

// Drive one full draw frame of the device with a synthetic input snapshot.
// Returns after ui.end() so focus bookkeeping settles exactly as in the game.
void drawFrame(CommsDevice& dev, x3::ui::UiContext& ui, CommsStubDevice& sd,
               const x3::ui::UiInput& in, float dt = 1.0f / 165.0f) {
    x3::rhi::FrameContext fc{};       // invalid: draws skipped, hit-tests live
    ui.begin(sd, fc, in);
    dev.draw(ui, sd, fc, dt);
    ui.end();
}

// A click at (mx,my) delivered as a rising edge.
x3::ui::UiInput clickAt(float mx, float my) {
    x3::ui::UiInput in{};
    in.mouseX = mx; in.mouseY = my;
    in.mouseDown = true; in.mousePressed = true;
    return in;
}

} // namespace

bool runShipCommsSelfTest() {
    g_pass = 0; g_fail = 0;
    x3::logInfo("=== SHIP COMMS DEVICE self-test (--test-comms) ===");

    // -----------------------------------------------------------------------
    // C1-C3 — THE STORE
    // -----------------------------------------------------------------------
    {
        CommsDevice d;
        check(d.size() == 0 && d.newest() == nullptr, "C1 a fresh device is empty");
        d.post(CommsSender::ShipAI, kCommsShipAiName, "systems nominal");
        d.post(CommsSender::Hostile, kCommsHostileName, "you are already dead");
        check(d.size() == 2, "C2 posts land in the feed");
        check(d.newest() != nullptr && d.newest()->sender == CommsSender::Hostile,
              "C3 newest() is the most recent post");
    }

    // -----------------------------------------------------------------------
    // C4-C5 — THE BACKLOG CAP (stated: 64 lines, oldest dropped)
    // -----------------------------------------------------------------------
    {
        CommsDevice d;
        for (int i = 0; i < kCommsBacklogCap * 3; ++i) {
            char t[32]; std::snprintf(t, sizeof(t), "line %d", i);
            d.post(CommsSender::ShipAI, "AEGIS", t);
        }
        check(d.size() == kCommsBacklogCap,
              "C4 the feed bounds its backlog at kCommsBacklogCap (64)");
        // The NEWEST line must be the survivor — dropping from the back during a
        // fight would discard the message that just arrived.
        check(d.newest() != nullptr &&
              d.newest()->text == std::string("line ") +
                  std::to_string(kCommsBacklogCap * 3 - 1),
              "C5 the cap drops the OLDEST line, never the newest");
    }

    // -----------------------------------------------------------------------
    // C6-C10 — THE FOCUS MODEL AND FLIGHT-INPUT RESTORATION.
    // This is the highest-risk requirement: a player must never lose the ability
    // to fly because the panel silently ate input.
    // -----------------------------------------------------------------------
    {
        CommsDevice d;
        check(!d.focused(), "C6 the device starts UNFOCUSED (flight owns the keys)");

        // The gate, driven through the same pure function HostShell calls.
        const bool flyBefore = commsFlightInputEnabled(false, false, false, d.focused());
        check(flyBefore, "C7 flight input is ENABLED while the device is unfocused");

        check(commsRouteKey(d, CommsKey::Focus) && d.focused(),
              "C8 the focus key focuses the device and is consumed");
        const bool flyDuring = commsFlightInputEnabled(false, false, false, d.focused());
        check(!flyDuring, "C9 flight input is DISABLED while the device is focused");

        // ESC must restore flight EXACTLY — same value as before focus, with no
        // other shell flag disturbed.
        check(commsRouteKey(d, CommsKey::Escape) && !d.focused(),
              "C10 ESC releases focus and is consumed");
        const bool flyAfter = commsFlightInputEnabled(false, false, false, d.focused());
        check(flyAfter == flyBefore && flyAfter,
              "C11 focus-out restores flight input EXACTLY to its pre-focus value");
    }

    // -----------------------------------------------------------------------
    // C12-C14 — KEY ROUTING. Unfocused, the device must be transparent to keys.
    // -----------------------------------------------------------------------
    {
        CommsDevice d;
        // Every flight-relevant key must fall THROUGH while unfocused.
        const CommsKey through[] = { CommsKey::ScrollUp, CommsKey::ScrollDown,
                                     CommsKey::NextFilter, CommsKey::Ack,
                                     CommsKey::Hail, CommsKey::Other,
                                     CommsKey::Escape };
        bool anyConsumed = false;
        for (CommsKey k : through) if (commsRouteKey(d, k)) anyConsumed = true;
        check(!anyConsumed,
              "C12 an UNFOCUSED device consumes NO key except focus (keys reach flight)");

        d.setFocused(true);
        bool allConsumed = true;
        for (CommsKey k : { CommsKey::ScrollUp, CommsKey::ScrollDown,
                            CommsKey::NextFilter, CommsKey::Other }) {
            if (!commsRouteKey(d, k)) allConsumed = false;
        }
        check(allConsumed && d.focused(),
              "C13 a FOCUSED device consumes its keys and stays focused");
        check(commsRouteKey(d, CommsKey::Focus) && !d.focused(),
              "C14 the focus key also TOGGLES back out (two ways to escape)");
    }

    // -----------------------------------------------------------------------
    // C15-C17 — THE BUTTONS FIRE THEIR ACTIONS (through the real UiContext).
    // -----------------------------------------------------------------------
    {
        CommsStubDevice sd;
        x3::ui::UiContext ui;
        CommsDevice d;
        d.post(CommsSender::Hostile, kCommsHostileName, "a line to acknowledge");
        d.post(CommsSender::ShipAI, kCommsShipAiName, "another one");
        check(d.unacked() == 2, "C15 posted lines start UNACKED");

        // Lay the panel out once to learn where its buttons actually are, then
        // click the ACK button's real rect — no hard-coded pixel guesses.
        x3::ui::UiInput idle{};
        drawFrame(d, ui, sd, idle);
        float px, py, pw, ph;
        d.lastRect(px, py, pw, ph);
        check(pw > 0.0f && px > 1920.0f * 0.5f,
              "C16 the panel is RIGHT-anchored and clear of the reticle at centre");

        // The button row sits at the bottom of the plate; ACK is the leftmost.
        const float btnY = py + ph - kHudPadY - 22.0f + 11.0f;
        const float ackX = px + kHudPadX + 20.0f;
        drawFrame(d, ui, sd, clickAt(ackX, btnY));
        check(d.unacked() == 0, "C17 clicking ACK acknowledges the feed");

        // HAIL is the second wide button.
        const int hailBefore = d.hailCount();
        const float availW = pw - kHudPadX * 2.0f;
        const float wideW  = (availW - 26.0f * 2.0f - 5.0f * 3.0f) * 0.5f;
        const float hailX  = px + kHudPadX + wideW + 5.0f + wideW * 0.5f;
        drawFrame(d, ui, sd, clickAt(hailX, btnY));
        check(d.hailCount() == hailBefore + 1,
              "C18 clicking HAIL fires its action (stub with a stated contract)");
    }

    // -----------------------------------------------------------------------
    // C19-C20 — THE IDLE HOST. A world with nothing to say must still draw.
    // -----------------------------------------------------------------------
    {
        CommsStubDevice sd;
        x3::ui::UiContext ui;
        CommsDevice d;
        CommsDirector dir;
        CommsSnapshot empty{};        // a host that publishes NOTHING
        dir.update(d, empty, 1.0f / 165.0f);
        check(d.size() == 0,
              "C19 a host with no content posts NOTHING (no lorem, no noise)");

        x3::ui::UiInput in{};
        drawFrame(d, ui, sd, in);     // must not crash, must produce a rect
        float px, py, pw, ph;
        d.lastRect(px, py, pw, ph);
        check(pw > 0.0f && ph > 0.0f,
              "C20 the device still DRAWS its idle surface with an empty feed");
    }

    // -----------------------------------------------------------------------
    // C21-C25 — WORMHOLE ADVISORIES: proximity + stability naming + hysteresis.
    // -----------------------------------------------------------------------
    {
        CommsDevice d;
        CommsDirector dir;
        CommsPortal portals[2];
        portals[0].name = "CRYSTAL CAVES"; portals[0].id = 1; portals[0].stable = true;
        portals[0].pos[0] = 0.0f; portals[0].pos[1] = 0.0f; portals[0].pos[2] = 0.0f;
        portals[1].name = "THE CLIFFS";    portals[1].id = 2; portals[1].stable = false;
        portals[1].pos[0] = 5000.0f; portals[1].pos[1] = 0.0f; portals[1].pos[2] = 0.0f;

        CommsSnapshot s{};
        s.portals = portals; s.portalCount = 2;
        // FAR from both: no advisory.
        s.playerPos[0] = -9000.0f;
        dir.update(d, s, 0.01f);
        check(d.size() == 0, "C21 no advisory fires while every wormhole is far off");

        // Close on the STABLE one.
        s.playerPos[0] = -100.0f;
        dir.update(d, s, 0.01f);
        check(d.size() == 1, "C22 an advisory fires when the player closes on a wormhole");
        const bool named = d.newest() &&
                           d.newest()->text.find("CRYSTAL CAVES") != std::string::npos;
        const bool saidStable = d.newest() &&
                                d.newest()->text.rfind("STABLE", 0) == 0;
        check(named && saidStable,
              "C23 the advisory NAMES the wormhole and reports it STABLE");
        check(d.newest() && d.newest()->sender == CommsSender::ShipAI,
              "C24 the advisory comes from the SHIP AI channel");

        // Sitting still must NOT re-fire (the 165 Hz machine-gun guard).
        for (int i = 0; i < 200; ++i) dir.update(d, s, 1.0f / 165.0f);
        check(d.size() == 1, "C25 an advisory does not repeat while parked in range");

        // Close on the UNSTABLE one; it must say so.
        s.playerPos[0] = 4900.0f;
        dir.update(d, s, 0.01f);
        const bool saidUnstable = d.newest() &&
                                  d.newest()->text.rfind("UNSTABLE", 0) == 0 &&
                                  d.newest()->text.find("THE CLIFFS") != std::string::npos;
        check(saidUnstable,
              "C26 an UNSTABLE wormhole is named and reported unstable");

        // Leaving the band and returning re-arms exactly once.
        s.playerPos[0] = -9000.0f;
        dir.update(d, s, 0.01f);
        const int afterLeave = d.size();
        s.playerPos[0] = -100.0f;
        dir.update(d, s, 0.01f);
        check(d.size() == afterLeave + 1,
              "C27 leaving the hysteresis band re-arms the advisory exactly once");
    }

    // -----------------------------------------------------------------------
    // C28-C33 — HOSTILE LINES FIRE ON THEIR REAL EVENTS, AND NOT OTHERWISE.
    // -----------------------------------------------------------------------
    {
        CommsDevice d;
        CommsDirector dir;
        CommsSnapshot s{};
        s.inSpace = true;

        // A quiet space frame: the AI greets once, the hostile channel is silent.
        dir.update(d, s, 0.01f);
        int hostiles = 0;
        for (int i = 0; i < d.size(); ++i)
            if (d.at(i).sender == CommsSender::Hostile) ++hostiles;
        check(hostiles == 0,
              "C28 NO hostile line fires on a quiet frame (events, not lorem)");

        // Idling for a long time must add nothing at all.
        const int quiet = d.size();
        for (int i = 0; i < 500; ++i) dir.update(d, s, 1.0f / 165.0f);
        check(d.size() == quiet, "C29 an unchanged snapshot posts nothing on repeat");

        // FIRST BLOOD.
        s.playerTookFirstHit = true;
        dir.update(d, s, 0.01f);
        check(d.newest() && d.newest()->sender == CommsSender::Hostile,
              "C30 first blood fires a HOSTILE taunt");
        const int afterBlood = d.size();
        dir.update(d, s, 0.01f);
        check(d.size() == afterBlood, "C31 first blood fires ONCE, not every frame");

        // A MOUNT SHEARED.
        s.mountsDestroyed = 1;
        dir.update(d, s, 0.01f);
        check(d.size() > afterBlood && d.newest()->sender == CommsSender::Hostile,
              "C32 shearing a mount fires a HOSTILE taunt");

        // BAYS LAUNCHING — the event the encounter already barks about.
        s.baysLaunching = true;
        const int beforeBays = d.size();
        dir.update(d, s, 0.01f);
        check(d.size() == beforeBays + 2,
              "C33 bays launching fires BOTH a hostile taunt and a ship-AI callout");

        // REACTOR BREACH.
        s.reactorBreach = true;
        const int beforeBreach = d.size();
        dir.update(d, s, 0.01f);
        check(d.size() == beforeBreach + 2, "C34 reactor breach fires its pair of lines");
    }

    // -----------------------------------------------------------------------
    // C35-C36 — SHIP-AI SYSTEMS CHATTER on real ship state, with hysteresis.
    // -----------------------------------------------------------------------
    {
        CommsDevice d;
        CommsDirector dir;
        CommsSnapshot s{};
        s.inSpace = true;
        dir.update(d, s, 0.01f);           // greeting
        const int base = d.size();

        s.shieldFrac = 0.20f;              // cross the low-shield threshold
        dir.update(d, s, 0.01f);
        check(d.size() == base + 1, "C35 crossing the low-shield threshold warns once");

        // Hovering on the threshold must not chatter.
        for (int i = 0; i < 300; ++i) { s.shieldFrac = 0.30f; dir.update(d, s, 1.0f / 165.0f); }
        check(d.size() == base + 1,
              "C36 sitting ON the threshold does not repeat the warning (hysteresis)");

        s.shieldFrac = 0.90f;              // recover past the upper band
        dir.update(d, s, 0.01f);
        check(d.size() == base + 2, "C37 recovering past the upper band reports recovery");
    }

    // -----------------------------------------------------------------------
    // C38-C40 — THE PUBLISH BUS.
    // -----------------------------------------------------------------------
    {
        CommsBus bus;
        CommsDevice d;
        CommsSnapshot snap{};
        bus.post(CommsSender::Hostile, "DREADNOUGHT", "routed through the bus");
        check(bus.pending() == 1, "C38 a bus post queues until the shell drains it");
        bus.drain(d, snap);
        check(d.size() == 1 && bus.pending() == 0,
              "C39 draining the bus moves queued lines into the device");

        // An un-drained bus must stay bounded (no leak when no shell is attached).
        for (int i = 0; i < kCommsBusQueueCap * 4; ++i)
            bus.post(CommsSender::ShipAI, "AEGIS", "spam");
        check(bus.pending() == kCommsBusQueueCap,
              "C40 an un-drained bus is BOUNDED (no unbounded growth headless)");
    }

    // -----------------------------------------------------------------------
    // C41 — dt-CORRECTNESS. The arrival glow must decay on wall time, not frames.
    // -----------------------------------------------------------------------
    {
        CommsDevice a, b;
        a.post(CommsSender::ShipAI, "AEGIS", "x");
        b.post(CommsSender::ShipAI, "AEGIS", "x");
        // Half a second of wall clock, at 60 Hz and at 165 Hz.
        for (int i = 0; i < 30;  ++i) a.update(1.0f / 60.0f);
        for (int i = 0; i < 82;  ++i) b.update(1.0f / 165.0f);
        check(std::fabs(a.arrivalGlow() - b.arrivalGlow()) < 0.02f,
              "C41 the arrival glow decays on dt, identically at 60 Hz and 165 Hz");
    }

    x3::logInfo("--test-comms: " + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
