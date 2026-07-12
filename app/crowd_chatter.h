#pragma once
// CROWD CHATTER — "Can we hear the people talk.. mumble.. see it in chat
// bubbles over their heads?" (Tim, 2026-07-11). Game/slice code only — engine/
// stays pure.
//
// A thin, deterministic layer over CrowdSystem (app/crowd.h) that gives the
// living NPCs a VOICE:
//   * CONVERSE pairs take turns "speaking" in rhythm with the existing
//     turn-taking gesture bobs: every 1.8-3.2 s the speaker flips, a 2-6 word
//     line from the authored LINE TABLE pops up in a chat bubble over the
//     speaker's head, and a short MURMUR walla take (assets/audio/crowd/,
//     baked by tools/gen_crowd_chatter.py) plays at the pair midpoint —
//     unintelligible mumbling, per-agent pitch variance. Several pairs near
//     each other on a street read as ambient walla.
//   * WORKERS grumble intermittently (bubble + a lower grumble take) and
//     grunt at task-loop points (crate hoist/drop — audio only).
//   * PLAYERS (kickabout) shout between passes; hand-game pairs mutter.
//
// LINE TABLE: a C++ table (crowd_chatter.cpp), not a data file — the lines are
// ~60 short strings of pure in-world flavor with zero authoring churn expected;
// a JSON loader would add a parse-failure lane and then need this exact table
// as its baked-in fallback anyway. If the writers ever want to iterate without
// a rebuild, lift the table to assets/world/chatter.json and keep this as the
// fallback.
//
// DETERMINISM: every choice (speaker order, turn lengths, line picks, solo
// cadences) comes from the crowd LCG pattern (1664525/1013904223) seeded from
// the venue seed + agent ids. No wall-clock randomness — two identical crowd
// sims produce identical chatter (asserted by the self-test).
//
// BUBBLE RULES (drawChatterBubbles): world-anchored over the speaker's head
// (the monster health-bar treatment: worldToScreen + drawHudQuad compositing),
// range <= ~14 m with a distance fade, LOS-culled through Layer::Static (the
// health-bar rayCast trick — no bubbles through walls), skipped entirely while
// the agent's room fails the PVS (scene.roomVisible), at most 4 drawn
// concurrently (nearest win), age fade-in/out. The HOST decides when the layer
// may draw at all (never over an open dialog/terminal/console UI).
//
// COST: fixed pools, zero per-frame heap allocation (the per-agent solo array
// is sized once per deployment build), all timers dt-scaled.

#include "crowd.h"

#include "engine/audio/IAudioSystem.h"
#include "engine/rhi/IRenderDevice.h"

#include <cstdint>

namespace x3::phys { class IPhysicsWorld; }

namespace x3::game {

// Which line tables a deployment speaks from.
enum class ChatterVenue : uint32_t {
    Facility = 0,   // detainee whispers + worker grumbles + hand-game mutters
    Street,         // civilian gossip + dock grumbles + kickabout shouts
    Club,           // one-liners over the music (reserved; not deployed yet)
};
const char* chatterVenueName(ChatterVenue v);

// The committed walla takes (invalid handles are fine — bubbles still show,
// the murmur is just silent; the clean-machine grace every cue has).
struct ChatterSounds {
    x3::audio::SoundHandle murmurA{};   // conversational take A
    x3::audio::SoundHandle murmurB{};   // brighter take B (alternates per turn)
    x3::audio::SoundHandle grumble{};   // low worker grumble/grunt take
};

// One live chat bubble. A slot is free when agent == kNoLink.
struct ChatterBubble {
    uint32_t    agent = kNoLink;   // index into the crowd's agents
    const char* line  = nullptr;   // points into the static line table
    float       age   = 0.0f;      // seconds since spawn
    float       ttl   = 0.0f;      // lifetime (fade-out eats the tail)
};

class CrowdChatter {
public:
    static constexpr uint32_t kMaxBubbles = 6;    // per-deployment slot pool
    static constexpr float    kBubbleRange = 14.0f; // draw range (m)
    static constexpr float    kAudioRange  = 20.0f; // murmur fire range (m)

    // Bind the venue + determinism seed. Call once per deployment (and again
    // after reset() when a streamed region rebuilds its crowd).
    void init(ChatterVenue venue, uint32_t seed);

    // Advance one frame over the (already-updated) crowd. `audio` may be null
    // (headless/self-test): events still count deterministically, nothing
    // plays. `eye` gates only the audible murmur fires (kAudioRange), never
    // the sim — determinism does not depend on the camera.
    void update(float dt, const CrowdSystem& crowd, x3::audio::IAudioSystem* audio,
                const ChatterSounds& snd, const x3::phys::Vec3& eye);

    // Forget everything (bubbles/pairs/solo timers) — the region-teardown
    // sibling of CrowdSystem::abandon(). init() state (venue/seed) survives.
    void reset();

    // ---- Queries (draw + self-test) ----
    uint32_t activeBubbles() const;
    const ChatterBubble& bubbleSlot(uint32_t i) const { return m_bubbles[i]; }
    uint32_t murmursFired() const  { return m_murmurs; }   // converse turns voiced
    uint32_t grumblesFired() const { return m_grumbles; }  // worker/player fires
    // Deterministic line pick (exposed so the self-test can assert stability).
    static const char* lineFor(ChatterVenue v, CrowdRole role, bool ballPlay,
                               uint32_t seed);

private:
    struct PairChat {
        uint32_t a = kNoLink, b = kNoLink;   // agent indices, a < b
        uint32_t speaker = kNoLink;          // whose turn it is
        float    turnT = 0.0f;               // countdown to the speaker flip
        uint32_t turn = 0;                   // exchange counter
        uint32_t rng = 0;                    // pair-local LCG state
    };
    struct Solo {
        float    t = -1.0f;                  // countdown to the next bark (<0 = unseeded)
        uint32_t lastPhase = 0xFFFFFFFFu;    // workPhase edge detector
    };
    static constexpr uint32_t kMaxPairs = 12;

    uint32_t rng();
    float    frand();   // 0..1
    void     spawnBubble(uint32_t agent, const char* line, float ttl);
    void     retireBubble(uint32_t agent);
    void     fireMurmur(x3::audio::IAudioSystem* audio, const ChatterSounds& snd,
                        const x3::phys::Vec3& at, const x3::phys::Vec3& eye,
                        uint32_t agentId, bool grumbleTake, bool brightTake,
                        float vol);

    ChatterVenue m_venue = ChatterVenue::Facility;
    uint32_t     m_seed = 1u;
    uint32_t     m_rngState = 1u;
    ChatterBubble m_bubbles[kMaxBubbles];
    PairChat      m_pairs[kMaxPairs];
    std::vector<Solo> m_solo;      // per-agent; sized once per crowd build
    uint32_t      m_murmurs = 0;
    uint32_t      m_grumbles = 0;
    uint32_t      m_logBudget = 4; // first fires logged (the audio-proof lines)
};

// Draw up to 4 nearest bubbles across all deployments (see BUBBLE RULES above).
// `physics` may be null (no LOS cull then — headless). Call from the HUD lane
// AFTER the 3D scene, only while no dialog/terminal/console UI is open.
struct ChatterDrawSite {
    const CrowdChatter* chatter = nullptr;
    const CrowdSystem*  crowd   = nullptr;
};
void drawChatterBubbles(x3::rhi::IRenderDevice& device,
                        const x3::rhi::FrameContext& frame,
                        x3::phys::IPhysicsWorld* physics, const Scene& scene,
                        const x3::phys::Vec3& eye,
                        const ChatterDrawSite* sites, uint32_t siteCount);

// Headless self-test section for --test-crowd (called from runCrowdSelfTest):
// (H1) a converse pair produces a bubble over one of its members; (H2) the
// speakers ALTERNATE across turns (both members speak, consecutive speakers
// differ); (H3) determinism — two identical crowd sims + same-seed chatters
// produce the identical bubble sequence, and lineFor() is stable; (H4) bubble
// lifetimes are bounded (age <= ttl <= kMaxLife) and bubbles die out after
// violence dissolves the chat; (H5) a deployment with no pairs/work/play stays
// silent forever (no bubbles, no audio events); (H6) murmur events fire per
// exchange turn (counted with a null audio system); (H7) the per-deployment
// bubble pool never exceeds kMaxBubbles; (H8) workers grumble on the job
// (worker bubbles + counted grunts). Returns true iff all pass.
bool runCrowdChatterSelfTest();

} // namespace x3::game
