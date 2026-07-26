// CROWD CHATTER implementation — see app/crowd_chatter.h for the design stance.
// Game/slice code only — engine/ stays pure.

#include "crowd_chatter.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

const char* chatterVenueName(ChatterVenue v) {
    switch (v) {
        case ChatterVenue::Facility: return "facility";
        case ChatterVenue::Street:   return "street";
        case ChatterVenue::Club:     return "club";
    }
    return "?";
}

namespace {

// ============================ THE LINE TABLE ================================
// 2-6 word PG snippets of pure in-world flavor (nouns from EFLZ_NARRATIVE.md +
// the ONE-WORLD map: the tower/breach, the Crash Site, New District, the
// Scrapyard, the docks, Club 1127, drones, Martinez). Baked C++ — see the
// header for why (a data file would need this exact table as its fallback).

// Facility detainees/staff — WHISPERS in the halls of Lab Zero.
constexpr const char* kFacilityWhisper[] = {
    "Keep your voice down.",
    "Cameras. Everywhere.",
    "They took another one.",
    "Floor two... don't ask.",
    "Heard the alarms last night?",
    "Someone opened the breach.",
    "Martinez is on edge.",
    "Rations again tonight.",
    "Don't look at the drones.",
    "Seven-Alpha is loose, they say.",
    "Maintenance re-keyed the stairwell after the incident.",
};
// Workers — grumbles over the crates / consoles / sweep lines (both venues).
constexpr const char* kWorkerGrumble[] = {
    "Another double shift.",
    "These crates never end.",
    "My back's done in.",
    "Who signs off on this?",
    "Console's acting up again.",
    "Five more, then break.",
    "This one's heavy.",
    "Scanner's down again.",
};
// Street civilians — sidewalk gossip in New District / the Scrapyard.
constexpr const char* kStreetGossip[] = {
    "Saw the crash come down.",
    "That tower gives me chills.",
    "New District rents, huh.",
    "Scrapyard's picked clean.",
    "Heard the club reopened.",
    "Code's 1127. Pass it on.",
    "Dock pay is late again.",
    "Stay off the boulevard.",
    "River's high this week.",
    "Nice night for it.",
};
// Kickabout knot — shouts between passes.
constexpr const char* kKickabout[] = {
    "Pass it!",
    "Here! Here!",
    "Top corner!",
    "Off the curb. Nice.",
    "Two-nil!",
    "My legs are gone.",
};
// Seated hand-game pair — quiet table talk.
constexpr const char* kHandGame[] = {
    "Your move.",
    "Best of five.",
    "You cheat.",
    "Again. One more.",
};
// Club 1127 — one-liners over the music (venue reserved for the club hosts).
constexpr const char* kClubLines[] = {
    "Love this track!",
    "DJ's on fire tonight!",
    "1127 forever!",
    "Can't hear you!",
};

// Bubble timing.
constexpr float kFadeIn   = 0.20f;
constexpr float kFadeOut  = 0.30f;
constexpr float kMaxLife  = 3.5f;   // no bubble outlives this (test-asserted)

inline float dist2XZ(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

} // namespace

const char* CrowdChatter::lineFor(ChatterVenue v, CrowdRole role, bool ballPlay,
                                  uint32_t seed) {
    const char* const* tab = kFacilityWhisper;
    uint32_t n = (uint32_t)(sizeof(kFacilityWhisper) / sizeof(kFacilityWhisper[0]));
    auto pick = [&](const char* const* t, size_t count) { tab = t; n = (uint32_t)count; };
    if (v == ChatterVenue::Club) {
        pick(kClubLines, sizeof(kClubLines) / sizeof(kClubLines[0]));
    } else if (role == CrowdRole::Worker) {
        pick(kWorkerGrumble, sizeof(kWorkerGrumble) / sizeof(kWorkerGrumble[0]));
    } else if (role == CrowdRole::Gamer) {
        if (ballPlay) pick(kKickabout, sizeof(kKickabout) / sizeof(kKickabout[0]));
        else          pick(kHandGame, sizeof(kHandGame) / sizeof(kHandGame[0]));
    } else if (v == ChatterVenue::Street) {
        pick(kStreetGossip, sizeof(kStreetGossip) / sizeof(kStreetGossip[0]));
    }
    return tab[(seed >> 8) % n];
}

uint32_t CrowdChatter::rng() {
    m_rngState = m_rngState * 1664525u + 1013904223u;   // the crowd LCG pattern
    return m_rngState;
}

float CrowdChatter::frand() { return (float)((rng() >> 8) % 10000) * 0.0001f; }

void CrowdChatter::init(ChatterVenue venue, uint32_t seed) {
    m_venue = venue;
    m_seed = seed ? seed : 1u;
    reset();
}

void CrowdChatter::reset() {
    for (auto& b : m_bubbles) b = ChatterBubble{};
    for (auto& p : m_pairs)   p = PairChat{};
    m_solo.clear();
    m_rngState = m_seed * 2654435761u + 0x9E3779B9u;
    m_murmurs = 0;
    m_grumbles = 0;
    m_logBudget = 4;
}

uint32_t CrowdChatter::activeBubbles() const {
    uint32_t n = 0;
    for (const auto& b : m_bubbles) if (b.agent != kNoLink) ++n;
    return n;
}

void CrowdChatter::spawnBubble(uint32_t agent, const char* line, float ttl) {
    ttl = std::min(ttl, kMaxLife);
    ChatterBubble* slot = nullptr;
    for (auto& b : m_bubbles) {
        if (b.agent == kNoLink) { slot = &b; break; }
    }
    if (!slot) {   // pool full: evict the oldest (nearest to death)
        float best = -1.0f;
        for (auto& b : m_bubbles)
            if (b.age > best) { best = b.age; slot = &b; }
    }
    slot->agent = agent;
    slot->line = line;
    slot->age = 0.0f;
    slot->ttl = ttl;
}

void CrowdChatter::retireBubble(uint32_t agent) {
    for (auto& b : m_bubbles)
        if (b.agent == agent) b.ttl = std::min(b.ttl, b.age + kFadeOut);
}

void CrowdChatter::fireMurmur(x3::audio::IAudioSystem* audio, const ChatterSounds& snd,
                              const x3::phys::Vec3& at, const x3::phys::Vec3& eye,
                              uint32_t agentId, bool grumbleTake, bool brightTake,
                              float vol) {
    if (!audio) return;
    const float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
    if (dx * dx + dy * dy + dz * dz > kAudioRange * kAudioRange) return;
    // Per-agent voice: a stable hash of the agent id spreads the pitches so a
    // street full of pairs reads as many voices, not one loop.
    const float u = (float)((agentId * 2654435761u >> 16) & 1023u) / 1023.0f;
    const x3::audio::SoundHandle h =
        grumbleTake ? snd.grumble : (brightTake ? snd.murmurB : snd.murmurA);
    const float pitch = grumbleTake ? (0.72f + 0.16f * u) : (0.86f + 0.26f * u);
    audio->playSound3D(h, at.x, at.y, at.z, vol, pitch);
}

void CrowdChatter::update(float dt, const CrowdSystem& crowd,
                          x3::audio::IAudioSystem* audio, const ChatterSounds& snd,
                          const x3::phys::Vec3& eye) {
    if (!crowd.built()) {
        // Crowd abandoned under us (region stream-out): forget live state.
        if (!m_solo.empty() || activeBubbles() > 0)
            for (auto& b : m_bubbles) b = ChatterBubble{};
        return;
    }
    const uint32_t n = crowd.agentCount();
    if (m_solo.size() != n) m_solo.assign(n, Solo{});   // once per crowd build

    // ---- Age + expire the bubbles ----
    for (auto& b : m_bubbles) {
        if (b.agent == kNoLink) continue;
        b.age += dt;
        if (b.age >= b.ttl) b = ChatterBubble{};
    }

    auto prand = [](PairChat& p) {
        p.rng = p.rng * 1664525u + 1013904223u;
        return p.rng;
    };
    const float scl = crowd.config().scale;

    // One exchange turn: flip (or seed) the speaker, pop their line bubble in
    // rhythm with the converse gesture bobs, murmur at the pair midpoint.
    auto startTurn = [&](PairChat& p, bool first) {
        if (first) p.speaker = ((prand(p) >> 8) & 1u) ? p.b : p.a;
        else       p.speaker = (p.speaker == p.a) ? p.b : p.a;
        p.turnT = 1.8f + (float)((prand(p) >> 8) % 1000) * 0.0014f;   // 1.8-3.2 s
        ++p.turn;
        const uint32_t listener = (p.speaker == p.a) ? p.b : p.a;
        retireBubble(listener);   // the partner yields the floor
        const char* line = lineFor(m_venue, CrowdRole::Civilian, false, prand(p));
        spawnBubble(p.speaker, line, p.turnT + kFadeOut);
        const CrowdAgent& sa = crowd.agent(p.speaker);
        const CrowdAgent& la = crowd.agent(listener);
        const x3::phys::Vec3 mid{ (sa.pos.x + la.pos.x) * 0.5f,
                                  sa.pos.y + 1.5f * scl,
                                  (sa.pos.z + la.pos.z) * 0.5f };
        ++m_murmurs;
        fireMurmur(audio, snd, mid, eye, p.speaker, false, (p.turn & 1u) != 0u, 0.30f);
        if (m_logBudget > 0) {
            --m_logBudget;
            x3::logInfo(std::string("[chatter] murmur fired: ") +
                        chatterVenueName(m_venue) + " pair(" + std::to_string(p.a) +
                        "," + std::to_string(p.b) + ") turn " + std::to_string(p.turn) +
                        " line=\"" + line + "\"");
        }
    };

    // ---- CONVERSE pairs: retire dissolved chats, adopt settled new ones ----
    for (auto& p : m_pairs) {
        if (p.a == kNoLink) continue;
        const bool ok = p.a < n && p.b < n &&
                        crowd.agent(p.a).state == CrowdState::Converse &&
                        crowd.agent(p.b).state == CrowdState::Converse &&
                        crowd.agent(p.a).partner == p.b &&
                        crowd.agent(p.b).partner == p.a;
        if (!ok) {
            retireBubble(p.a);
            retireBubble(p.b);
            p = PairChat{};
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        const CrowdAgent& A = crowd.agent(i);
        if (A.state != CrowdState::Converse) continue;
        const uint32_t j = A.partner;
        if (j == kNoLink || j >= n || j <= i) continue;   // canonical i < j
        const CrowdAgent& B = crowd.agent(j);
        if (B.state != CrowdState::Converse || B.partner != i) continue;
        // Wait until BOTH stand in their talk slots (the bubble starts when the
        // chat visually starts, not while they walk over).
        if (dist2XZ(A.pos, A.target) > 0.09f || dist2XZ(B.pos, B.target) > 0.09f)
            continue;
        bool known = false;
        for (const auto& p : m_pairs)
            if (p.a == i && p.b == j) { known = true; break; }
        if (known) continue;
        for (auto& p : m_pairs) {
            if (p.a != kNoLink) continue;
            p.a = i; p.b = j; p.turn = 0;
            p.rng = (m_seed * 2654435761u) ^ (i * 73856093u) ^ (j * 19349663u);
            if (p.rng == 0) p.rng = 0x9E3779B9u;
            startTurn(p, /*first=*/true);
            break;
        }
    }
    // Tick the live exchanges.
    for (auto& p : m_pairs) {
        if (p.a == kNoLink) continue;
        p.turnT -= dt;
        if (p.turnT <= 0.0f) startTurn(p, /*first=*/false);
    }

    // ---- WORKERS grumble / PLAYERS shout (intermittent solo barks) ----
    for (uint32_t i = 0; i < n; ++i) {
        const CrowdAgent& a = crowd.agent(i);
        Solo& s = m_solo[i];
        if (a.role == CrowdRole::Worker && a.state == CrowdState::Work) {
            // Task-loop grunts: the crate hoist/drop micro-phase edges
            // (crowd.cpp kCarryHoist=1 / kCarryDrop=3), coin-flipped so not
            // every lift is voiced.
            if (a.workPhase != s.lastPhase) {
                const bool edge = s.lastPhase != 0xFFFFFFFFu &&
                                  (a.workPhase == 1u || a.workPhase == 3u);
                s.lastPhase = a.workPhase;
                if (edge && ((rng() >> 8) & 1u)) {
                    ++m_grumbles;
                    const x3::phys::Vec3 head{ a.pos.x, a.pos.y + 1.5f * scl, a.pos.z };
                    fireMurmur(audio, snd, head, eye, i, true, false, 0.26f);
                }
            }
            if (s.t < 0.0f) s.t = 4.0f + frand() * 10.0f;   // stagger the first
            s.t -= dt;
            if (s.t <= 0.0f) {
                s.t = 9.0f + frand() * 11.0f;               // 9-20 s cadence
                spawnBubble(i, lineFor(m_venue, CrowdRole::Worker, false, rng()), 2.8f);
                ++m_grumbles;
                const x3::phys::Vec3 head{ a.pos.x, a.pos.y + 1.5f * scl, a.pos.z };
                fireMurmur(audio, snd, head, eye, i, true, false, 0.32f);
            }
        } else if (a.role == CrowdRole::Gamer && a.state == CrowdState::Play) {
            const bool ball = a.playIdx < (uint32_t)crowd.config().play.size() &&
                              crowd.config().play[a.playIdx].ball;
            if (s.t < 0.0f) s.t = 3.0f + frand() * 8.0f;
            s.t -= dt;
            if (s.t <= 0.0f) {
                s.t = (ball ? 6.0f : 9.0f) + frand() * 9.0f;
                spawnBubble(i, lineFor(m_venue, CrowdRole::Gamer, ball, rng()), 2.4f);
                ++m_grumbles;
                const x3::phys::Vec3 head{ a.pos.x, a.pos.y + 1.5f * scl, a.pos.z };
                // Kickabout shouts ride the bright take a touch sped up;
                // hand-game mutters use the low grumble.
                fireMurmur(audio, snd, head, eye, i, !ball, ball, 0.32f);
            }
        }
        // Other states (scatter/cower/idle) keep their timers frozen — a
        // frightened crowd goes quiet, then picks the thread back up.
    }
}

// ===========================================================================
// Bubble rendering — the monster health-bar treatment (worldToScreen +
// drawHudQuad compositing + rayCast LOS), a rounded dark slab + tail + the
// line in the Menu font.
// ===========================================================================

void drawChatterBubbles(x3::rhi::IRenderDevice& device,
                        const x3::rhi::FrameContext& frame,
                        x3::phys::IPhysicsWorld* physics, const Scene& scene,
                        const x3::phys::Vec3& eye,
                        const ChatterDrawSite* sites, uint32_t siteCount) {
    struct Cand { float d2, sx, sy, alpha; const char* line; };
    constexpr uint32_t kMaxCand = 24;
    constexpr uint32_t kMaxDrawn = 4;    // hard concurrent-bubble cap
    Cand cands[kMaxCand];
    uint32_t nc = 0;

    for (uint32_t si = 0; si < siteCount; ++si) {
        const CrowdChatter* ch = sites[si].chatter;
        const CrowdSystem*  cs = sites[si].crowd;
        if (!ch || !cs || !cs->built()) continue;
        if (!scene.roomVisible(cs->config().roomId)) continue;   // PVS gate
        const float scl = cs->config().scale;
        for (uint32_t i = 0; i < CrowdChatter::kMaxBubbles; ++i) {
            const ChatterBubble& b = ch->bubbleSlot(i);
            if (b.agent == kNoLink || !b.line || b.agent >= cs->agentCount())
                continue;
            const CrowdAgent& a = cs->agent(b.agent);
            // Anchor over the head; crouch-aware so a seated hand-gamer's
            // bubble doesn't float in the air.
            const x3::phys::Vec3 head{ a.pos.x,
                                       a.pos.y + (1.72f * a.visCrouch + 0.28f) * scl,
                                       a.pos.z };
            const float dx = head.x - eye.x, dy = head.y - eye.y, dz = head.z - eye.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > CrowdChatter::kBubbleRange * CrowdChatter::kBubbleRange)
                continue;
            const float dist = std::sqrt(d2);
            // Distance fade (full inside 10 m, gone at 14) x age fade in/out.
            const float distA = std::clamp((CrowdChatter::kBubbleRange - dist) / 4.0f,
                                           0.0f, 1.0f);
            const float lifeA = std::clamp(std::min(b.age / 0.20f,
                                                    (b.ttl - b.age) / 0.30f),
                                           0.0f, 1.0f);
            const float alpha = distA * lifeA;
            if (alpha <= 0.02f) continue;
            // LOS cull (the health-bar trick): a static wall between the eye
            // and the speaker's head hides the bubble.
            if (physics && dist > 0.001f) {
                const x3::phys::Vec3 nd{ dx / dist, dy / dist, dz / dist };
                const x3::phys::RayHit los =
                    physics->rayCast(eye, nd, dist - 0.3f, x3::phys::Layer::Static);
                if (los.hit) continue;
            }
            float sx = 0.0f, sy = 0.0f;
            if (!device.worldToScreen(head.x, head.y, head.z, sx, sy)) continue;
            if (nc < kMaxCand) cands[nc++] = { d2, sx, sy, alpha, b.line };
        }
    }
    if (nc == 0) return;
    // Nearest-first (tiny insertion sort — nc <= 24).
    for (uint32_t i = 1; i < nc; ++i) {
        const Cand c = cands[i];
        uint32_t j = i;
        while (j > 0 && cands[j - 1].d2 > c.d2) { cands[j] = cands[j - 1]; --j; }
        cands[j] = c;
    }
    uint32_t hw = 0, hh = 0;
    device.hudSize(hw, hh);
    const uint32_t nDraw = std::min(nc, kMaxDrawn);
    // Farthest of the kept set first so the nearest bubble composites on top.
    for (int k = (int)nDraw - 1; k >= 0; --k) {
        const Cand& c = cands[k];
        const float px = 15.0f;                              // Menu-font cap height
        const float tw = device.textAdvance(x3::rhi::FontRole::Menu, c.line, px);
        const float w = tw + 18.0f, h = 26.0f;
        float x0 = c.sx - w * 0.5f;
        float y0 = c.sy - h - 12.0f;                         // slab + tail over the head
        if (x0 < 4.0f) x0 = 4.0f;
        if (hw > 8 && x0 + w > (float)hw - 4.0f) x0 = (float)hw - 4.0f - w;
        if (y0 < 4.0f) y0 = 4.0f;
        if (hh > 44 && y0 > (float)hh - 44.0f) y0 = (float)hh - 44.0f;
        const float a = c.alpha;
        const float rim[4]   = { 0.55f, 0.62f, 0.72f, 0.50f * a };
        const float panel[4] = { 0.045f, 0.055f, 0.085f, 0.88f * a };
        // Rounded dark slab: two overlapping quads cut the corners (3 px
        // radius read); the rim repeats the pair 1.5 px larger underneath.
        device.drawHudQuad(frame, x0 - 1.5f, y0 + 1.5f, w + 3.0f, h - 3.0f, rim);
        device.drawHudQuad(frame, x0 + 1.5f, y0 - 1.5f, w - 3.0f, h + 3.0f, rim);
        device.drawHudQuad(frame, x0, y0 + 3.0f, w, h - 6.0f, panel);
        device.drawHudQuad(frame, x0 + 3.0f, y0, w - 6.0f, h, panel);
        // Tail stepping down toward the head.
        device.drawHudQuad(frame, c.sx - 5.0f, y0 + h, 10.0f, 4.0f, panel);
        device.drawHudQuad(frame, c.sx - 2.5f, y0 + h + 4.0f, 5.0f, 3.0f, panel);
        // The line: Menu font, near-white ink over a soft shadow.
        const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.75f * a };
        const float ink[4]    = { 0.93f, 0.96f, 1.00f, a };
        const float tx = x0 + (w - tw) * 0.5f, ty = y0 + 6.0f;
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, c.line,
                            tx + 1.2f, ty + 1.2f, px, shadow);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, c.line, tx, ty, px, ink);
    }
}

// ===========================================================================
// Headless self-test section (--test-crowd, H1..H8)
// ===========================================================================

namespace {

int h_pass = 0, h_fail = 0;
void hcheck(bool cond, const char* name) {
    if (cond) { ++h_pass; x3::logInfo(std::string("[chatter-test] PASS ") + name); }
    else      { ++h_fail; x3::logError(std::string("[chatter-test] FAIL ") + name); }
}

CrowdConfig chatterTestConfig() {
    CrowdConfig cfg;
    cfg.count = 8;
    cfg.centerX = 0.0f; cfg.centerZ = 0.0f; cfg.groundY = 0.0f;
    cfg.radius = 18.0f;
    cfg.converse = true;
    cfg.points = { -4.0f, 0.0f,  4.0f, 0.0f,  0.0f, 4.0f,  0.0f, -4.0f };
    return cfg;
}

} // namespace

bool runCrowdChatterSelfTest() {
    h_pass = h_fail = 0;
    const float dt = 1.0f / 60.0f;
    const x3::phys::Vec3 eye{ 0.0f, 1.7f, 8.0f };
    const ChatterSounds noSnd{};   // invalid handles; audio is null anyway

    // ---- Twin deployments for H1/H2/H3/H4/H6/H7: identical crowds + chatters
    // ticked in LOCKSTEP; sim A is observed, sim B is the determinism mirror.
    HeadlessRenderDevice devA, devB;
    Scene sA, sB;
    CrowdSystem cA, cB;
    const CrowdConfig cfg = chatterTestConfig();
    cA.build(cfg, sA, devA);
    cB.build(cfg, sB, devB);
    CrowdChatter chA, chB;
    chA.init(ChatterVenue::Street, 7u);
    chB.init(ChatterVenue::Street, 7u);

    bool mirrored = true;         // H3: every frame's bubble set identical
    bool lifeBounded = true;      // H4: age <= ttl <= kMaxLife
    uint32_t maxActive = 0;       // H7: pool cap
    // H2 speaker log: fresh bubbles for the FIRST adopted pair.
    uint32_t pairA = kNoLink, pairB = kNoLink;
    bool pairDone = false;   // stop recording once THAT chat dissolves
    uint32_t speakers[64]; uint32_t nSpeak = 0;
    float prevAge[CrowdChatter::kMaxBubbles];
    uint32_t prevAgent[CrowdChatter::kMaxBubbles];
    for (uint32_t i = 0; i < CrowdChatter::kMaxBubbles; ++i) {
        prevAge[i] = 0.0f; prevAgent[i] = kNoLink;
    }

    const int kFrames = 60 * 120;   // 2 min of simulated street life
    for (int f = 0; f < kFrames; ++f) {
        cA.update(dt, sA);
        cB.update(dt, sB);
        chA.update(dt, cA, nullptr, noSnd, eye);
        chB.update(dt, cB, nullptr, noSnd, eye);
        maxActive = std::max(maxActive, chA.activeBubbles());
        // The locked pair's chat ended? Freeze the speaker log (a later RE-pair
        // with a different partner must not pollute the alternation check).
        if (pairA != kNoLink && !pairDone) {
            const CrowdAgent& pa = cA.agent(pairA);
            if (pa.state != CrowdState::Converse || pa.partner != pairB)
                pairDone = true;
        }
        for (uint32_t i = 0; i < CrowdChatter::kMaxBubbles; ++i) {
            const ChatterBubble& a = chA.bubbleSlot(i);
            const ChatterBubble& b = chB.bubbleSlot(i);
            if (a.agent != b.agent || a.line != b.line) mirrored = false;
            if (a.agent != kNoLink) {
                if (a.age > a.ttl + 0.05f || a.ttl > 3.6f) lifeBounded = false;
                // Fresh spawn in this slot?
                const bool fresh = (prevAgent[i] != a.agent) || (a.age < prevAge[i]);
                if (fresh) {
                    // Lock onto the first conversing pair we see.
                    if (pairA == kNoLink) {
                        const CrowdAgent& ag = cA.agent(a.agent);
                        if (ag.state == CrowdState::Converse && ag.partner != kNoLink) {
                            pairA = a.agent;
                            pairB = ag.partner;
                        }
                    }
                    if (!pairDone && (a.agent == pairA || a.agent == pairB) &&
                        nSpeak < 64)
                        speakers[nSpeak++] = a.agent;
                }
                prevAge[i] = a.age; prevAgent[i] = a.agent;
            } else {
                prevAge[i] = 0.0f; prevAgent[i] = kNoLink;
            }
        }
    }

    // ---- H1: a converse pair produced a bubble over one of its members ----
    hcheck(pairA != kNoLink && nSpeak >= 1,
           "H1 converse pair produces a speaker bubble");

    // ---- H2: speakers ALTERNATE (both spoke; consecutive turns differ) ----
    {
        bool bothSpoke = false, alternating = nSpeak >= 2;
        bool sawA = false, sawB = false;
        for (uint32_t i = 0; i < nSpeak; ++i) {
            if (speakers[i] == pairA) sawA = true;
            if (speakers[i] == pairB) sawB = true;
            if (i > 0 && speakers[i] == speakers[i - 1]) alternating = false;
        }
        bothSpoke = sawA && sawB;
        hcheck(nSpeak >= 2 && bothSpoke && alternating,
               "H2 speakers alternate across exchange turns");
    }

    // ---- H3: determinism — lockstep twin produced the identical bubble
    // sequence; lineFor() is stable for a fixed seed ----
    {
        const char* l1 = CrowdChatter::lineFor(ChatterVenue::Street,
                                               CrowdRole::Civilian, false, 12345u);
        const char* l2 = CrowdChatter::lineFor(ChatterVenue::Street,
                                               CrowdRole::Civilian, false, 12345u);
        hcheck(mirrored && l1 == l2 && l1 != nullptr,
               "H3 deterministic chatter (twin sims mirror; lineFor stable)");
    }

    // ---- H4: lifetimes bounded; violence silences the chat ----
    {
        cA.onViolence(x3::phys::Vec3{ 0.0f, 0.0f, 0.0f });
        for (int f = 0; f < 60 * 2; ++f) {   // 2 s: fades (0.3 s) fully clear
            cA.update(dt, sA);
            chA.update(dt, cA, nullptr, noSnd, eye);
        }
        hcheck(lifeBounded && chA.activeBubbles() == 0,
               "H4 bubble lifetimes bounded; violence dissolves the chat to silence");
    }

    // ---- H6: murmur events fired per exchange turn (null audio counted) ----
    hcheck(chA.murmursFired() >= 3,
           "H6 murmur audio events fire per exchange turn");

    // ---- H7: the bubble pool never exceeded its cap ----
    hcheck(maxActive >= 1 && maxActive <= CrowdChatter::kMaxBubbles,
           "H7 concurrent bubbles capped at the pool size");

    // ---- H5: a deployment with NO pairs/work/play stays silent forever ----
    {
        HeadlessRenderDevice dev5;
        Scene s5;
        CrowdSystem c5;
        CrowdConfig cfg5 = chatterTestConfig();
        cfg5.converse = false;   // wander-only civilians: nobody to talk to
        c5.build(cfg5, s5, dev5);
        CrowdChatter ch5;
        ch5.init(ChatterVenue::Street, 3u);
        bool everBubbled = false;
        for (int f = 0; f < 60 * 60; ++f) {
            c5.update(dt, s5);
            ch5.update(dt, c5, nullptr, noSnd, eye);
            if (ch5.activeBubbles() > 0) everBubbled = true;
        }
        hcheck(!everBubbled && ch5.murmursFired() == 0 && ch5.grumblesFired() == 0,
               "H5 silence with no pair (no bubbles, no audio events)");
    }

    // ---- H8: workers grumble on the job (bubble + counted audio event) ----
    {
        HeadlessRenderDevice dev8;
        Scene s8;
        CrowdSystem c8;
        CrowdConfig cfg8;
        cfg8.count = 3;
        cfg8.centerX = 0.0f; cfg8.centerZ = 0.0f; cfg8.groundY = 0.0f;
        cfg8.radius = 20.0f;
        CrowdWorkPoint carry;
        carry.kind = CrowdWorkPoint::Kind::Carry;
        carry.ax = -6.0f; carry.az = 0.0f; carry.bx = 6.0f; carry.bz = 0.0f;
        CrowdWorkPoint sweep;
        sweep.kind = CrowdWorkPoint::Kind::Sweep;
        sweep.ax = -6.0f; sweep.az = 4.0f; sweep.bx = 6.0f; sweep.bz = 4.0f;
        cfg8.work = { carry, sweep };
        c8.build(cfg8, s8, dev8);
        CrowdChatter ch8;
        ch8.init(ChatterVenue::Facility, 11u);
        bool workerBubbled = false;
        for (int f = 0; f < 60 * 90; ++f) {
            c8.update(dt, s8);
            ch8.update(dt, c8, nullptr, noSnd, eye);
            for (uint32_t i = 0; i < CrowdChatter::kMaxBubbles && !workerBubbled; ++i) {
                const ChatterBubble& b = ch8.bubbleSlot(i);
                if (b.agent != kNoLink &&
                    c8.agent(b.agent).role == CrowdRole::Worker)
                    workerBubbled = true;
            }
        }
        hcheck(workerBubbled && ch8.grumblesFired() >= 2,
               "H8 workers grumble on the job (bubbles + counted grunts)");
    }

    x3::logInfo("chatter: " + std::to_string(h_pass) + "/" +
                std::to_string(h_pass + h_fail) + " passed");
    return h_fail == 0;
}

} // namespace x3::game
